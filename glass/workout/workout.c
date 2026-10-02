#include "gm_plugin_lvgl_api.h"
#include "gm_plugin_libc.h"

/* Paired with PhoneSDK/examples/workout. A guided strength-workout coach.
 * The phone builds a training program (exercises with reps-or-time, sets and
 * rest) and starts the session; the glasses step through it hands-free: the
 * current exercise and set are shown big, the button clears a set (and skips
 * the rest timer), a long press resets. When the last set is cleared the
 * glasses report the finished session back to the phone for its log. */

#define SETTINGS_CHANNEL UINT16_C(0x574B) /* 'WK': phone -> glasses */
#define EVENT_CHANNEL UINT16_C(0x574C)    /* 'WL': glasses -> phone */
#define PROTOCOL_VERSION 1U

#define COMMAND_PROGRAM 1U
#define COMMAND_START 2U
#define COMMAND_PAUSE 3U
#define COMMAND_STOP 4U
#define COMMAND_SKIP 5U

#define EVENT_WORKOUT_DONE UINT8_C(1)

#define MODE_REPS 0U
#define MODE_TIME 1U

#define MAX_EXERCISES 10U
#define NAME_MAX 16U
#define MIN_TARGET 1
#define MAX_REPS 99
#define MAX_TIME_SEC 300
#define MIN_SETS 1
#define MAX_SETS 20
#define MAX_REST_SEC 300

#define REFRESH_INTERVAL_MS 100U

#define SCREEN_MARGIN 20
#define EX_NAME_HEIGHT 40
#define SERIES_HEIGHT 24
#define TARGET_HEIGHT 44
#define HINT_HEIGHT 30

typedef struct {
    char name[NAME_MAX + 1];
    uint8_t mode;
    uint16_t target;
    uint8_t sets;
    uint16_t rest_sec;
} workout_ex_t;

typedef struct {
    const gm_plugin_host_api_t *host;
    const gm_plugin_lvgl_api_t *ui;
    const gm_plugin_libc_extension_api_t *libc;
    gm_plugin_lvgl_obj_t *screen;
    gm_plugin_lvgl_obj_t *title_label;
    gm_plugin_lvgl_obj_t *ex_label;
    gm_plugin_lvgl_obj_t *series_label;
    gm_plugin_lvgl_obj_t *target_label;
    gm_plugin_lvgl_obj_t *info_label;
    gm_plugin_lvgl_obj_t *hint_label;

    workout_ex_t exercises[MAX_EXERCISES];
    uint8_t count;

    uint8_t ex_index;
    uint8_t set_index; /* 1-based within the current exercise */
    bool resting;
    bool running;
    bool paused;
    bool done;

    uint32_t phase_remaining_ms; /* time left in a TIME set or in a rest */
    uint32_t session_elapsed_ms;
    uint32_t refresh_elapsed_ms;
    uint16_t sets_done;
    uint8_t language; /* 0=fr 1=en 2=es 3=it 4=de 5=zh-CN */
} workout_t;

#define L(fr_str, en_str, es_str, it_str, de_str, zh_str) \
    (wo.language == 1U ? (en_str) : wo.language == 2U ? (es_str) : \
     wo.language == 3U ? (it_str) : wo.language == 4U ? (de_str) : \
     wo.language == 5U ? (zh_str) : (fr_str))

static workout_t wo;

static uint8_t workout_detect_language(const char *locale)
{
    if (locale[0] == 'e' && locale[1] == 'n') return 1U;
    if (locale[0] == 'e' && locale[1] == 's') return 2U;
    if (locale[0] == 'i' && locale[1] == 't') return 3U;
    if (locale[0] == 'd' && locale[1] == 'e') return 4U;
    if (locale[0] == 'z' && locale[1] == 'h') return 5U;
    return 0U;
}

#define number gm_plugin_lvgl_style_number
#define color gm_plugin_lvgl_style_color

static int32_t clamp_i32(int32_t value, int32_t minimum, int32_t maximum)
{
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static void set_style(gm_plugin_lvgl_obj_t *object,
                       gm_plugin_lvgl_style_prop_t property,
                       gm_plugin_lvgl_style_value_t value)
{
    wo.ui->style_set(object, property, value, GM_PLUGIN_LVGL_SELECTOR_MAIN);
}

static void style_panel(gm_plugin_lvgl_obj_t *object, uint8_t fill,
                         uint8_t border, uint8_t radius)
{
    set_style(object, GM_PLUGIN_LVGL_STYLE_BG_COLOR, color(fill));
    set_style(object, GM_PLUGIN_LVGL_STYLE_BG_OPA, number(255));
    set_style(object, GM_PLUGIN_LVGL_STYLE_BORDER_COLOR, color(0xA0));
    set_style(object, GM_PLUGIN_LVGL_STYLE_BORDER_OPA, number(255));
    set_style(object, GM_PLUGIN_LVGL_STYLE_BORDER_WIDTH, number(border));
    set_style(object, GM_PLUGIN_LVGL_STYLE_RADIUS, number(radius));
    wo.ui->obj_clear_flag(object, GM_PLUGIN_LVGL_FLAG_SCROLLABLE);
}

static gm_plugin_lvgl_obj_t *make_label(gm_plugin_lvgl_obj_t *parent,
                                         const char *text, uint8_t shade)
{
    gm_plugin_lvgl_obj_t *label = wo.ui->label_create(parent);
    if (label == 0) return 0;
    wo.ui->label_set_text(label, text);
    wo.ui->label_set_long_mode(label, GM_PLUGIN_LVGL_LABEL_DOT);
    set_style(label, GM_PLUGIN_LVGL_STYLE_TEXT_COLOR, color(shade));
    set_style(label, GM_PLUGIN_LVGL_STYLE_TEXT_ALIGN,
              number(GM_PLUGIN_LVGL_TEXT_ALIGN_CENTER));
    return label;
}

static const char *exercise_name(uint8_t index)
{
    if (index >= wo.count) return "-";
    return wo.exercises[index].name;
}

/* Human-readable target, e.g. "12" or "0:45". */
static void format_target(char *buffer, uint32_t size, const workout_ex_t *ex)
{
    if (ex->mode == MODE_TIME) {
        uint32_t seconds = (uint32_t)ex->target;
        wo.libc->snprintf(buffer, size, "%u:%02u",
                          (unsigned int)(seconds / 60U),
                          (unsigned int)(seconds % 60U));
    } else {
        wo.libc->snprintf(buffer, size, "%u", (unsigned int)ex->target);
    }
}

static void refresh_display(void)
{
    char text[48];
    char target[16];

    if (wo.count == 0U) {
        wo.ui->label_set_text(wo.ex_label, L("Aucun programme", "No program", "Sin programa", "Nessun programma", "Kein Programm", "无训练计划"));
        wo.ui->label_set_text(wo.series_label, "");
        wo.ui->label_set_text(wo.target_label, "");
        wo.ui->label_set_text(wo.info_label,
            L("Choisis une seance sur le telephone",
              "Pick a workout on the phone",
              "Elige una sesión en el teléfono",
              "Scegli un allenamento sul telefono",
              "Wähle ein Training am Telefon",
              "请在手机上选择训练"));
        wo.ui->label_set_text(wo.hint_label, "");
        return;
    }

    if (wo.done) {
        wo.ui->label_set_text(wo.ex_label, L("Termine", "Done", "Terminado", "Finito", "Fertig", "完成"));
        set_style(wo.ex_label, GM_PLUGIN_LVGL_STYLE_TEXT_COLOR, color(0xFF));
        wo.ui->label_set_text(wo.series_label, "");
        wo.ui->label_set_text(wo.target_label, "");
        wo.libc->snprintf(text, sizeof(text), L("Series : %u", "Sets: %u", "Series: %u", "Serie: %u", "Sätze: %u", "组：%u"),
                          (unsigned int)wo.sets_done);
        wo.ui->label_set_text(wo.info_label, text);
        wo.ui->label_set_text(wo.hint_label,
            L("Bouton : recommencer", "Button: restart", "Botón: reiniciar", "Pulsante: riavvia", "Taste: neu starten", "按钮：重新开始"));
        return;
    }

    if (!wo.running) {
        wo.ui->label_set_text(wo.ex_label,
            wo.exercises[0].name[0] != '\0'
                ? wo.exercises[0].name : L("Pret", "Ready", "Listo", "Pronto", "Bereit", "准备"));
        set_style(wo.ex_label, GM_PLUGIN_LVGL_STYLE_TEXT_COLOR, color(0xC0));
        format_target(target, sizeof(target), &wo.exercises[0]);
        wo.libc->snprintf(text, sizeof(text), L("%u series x %s",
                          "%u sets x %s",
                          "%u series x %s",
                          "%u serie x %s",
                          "%u Sätze x %s",
                          "%u 组 x %s"),
                          (unsigned int)wo.exercises[0].sets, target);
        wo.ui->label_set_text(wo.series_label, text);
        wo.ui->label_set_text(wo.target_label, "");
        wo.libc->snprintf(text, sizeof(text), L("Exercices : %u",
                          "Exercises: %u",
                          "Ejercicios: %u",
                          "Esercizi: %u",
                          "Übungen: %u",
                          "动作：%u"),
                          (unsigned int)wo.count);
        wo.ui->label_set_text(wo.info_label, text);
        wo.ui->label_set_text(wo.hint_label,
            L("Bouton : demarrer", "Button: start", "Botón: iniciar", "Pulsante: avvia", "Taste: Start", "按钮：开始"));
        return;
    }

    if (wo.paused) {
        set_style(wo.ex_label, GM_PLUGIN_LVGL_STYLE_TEXT_COLOR, color(0xD0));
        wo.ui->label_set_text(wo.series_label, L("En pause", "Paused", "En pausa", "In pausa", "Pausiert", "暂停"));
        wo.ui->label_set_text(wo.info_label, "");
        wo.ui->label_set_text(wo.hint_label,
            L("Bouton : reprendre", "Button: resume", "Botón: reanudar", "Pulsante: riprendi", "Taste: Weiter", "按钮：继续"));
        return;
    }

    if (wo.resting) {
        uint32_t seconds = (wo.phase_remaining_ms + 999U) / 1000U;
        wo.ui->label_set_text(wo.ex_label, L("Repos", "Rest", "Descanso", "Riposo", "Pause", "休息"));
        set_style(wo.ex_label, GM_PLUGIN_LVGL_STYLE_TEXT_COLOR, color(0xC0));
        wo.libc->snprintf(text, sizeof(text), "%u:%02u",
                          (unsigned int)(seconds / 60U),
                          (unsigned int)(seconds % 60U));
        wo.ui->label_set_text(wo.target_label, text);
        wo.libc->snprintf(text, sizeof(text), L("Suivant : %s",
                          "Next: %s",
                          "Siguiente: %s",
                          "Prossimo: %s",
                          "Nächste: %s",
                          "下一个：%s"),
                          exercise_name(wo.ex_index));
        wo.ui->label_set_text(wo.info_label, text);
        wo.ui->label_set_text(wo.hint_label,
            L("Bouton : passer le repos", "Button: skip rest", "Botón: saltar descanso", "Pulsante: salta riposo", "Taste: Pause überspringen", "按钮：跳过休息"));
        return;
    }

    {
        const workout_ex_t *ex = &wo.exercises[wo.ex_index];
        set_style(wo.ex_label, GM_PLUGIN_LVGL_STYLE_TEXT_COLOR, color(0xFF));
        wo.ui->label_set_text(wo.ex_label, ex->name);
        wo.libc->snprintf(text, sizeof(text),
                          L("Exercice %u/%u    Serie %u/%u",
                            "Exercise %u/%u    Set %u/%u",
                            "Ejercicio %u/%u    Serie %u/%u",
                            "Esercizio %u/%u    Serie %u/%u",
                            "Übung %u/%u    Satz %u/%u",
                            "动作 %u/%u    组 %u/%u"),
                          (unsigned int)(wo.ex_index + 1U), (unsigned int)wo.count,
                          (unsigned int)wo.set_index, (unsigned int)ex->sets);
        wo.ui->label_set_text(wo.series_label, text);

        if (ex->mode == MODE_TIME) {
            uint32_t seconds = (wo.phase_remaining_ms + 999U) / 1000U;
            wo.libc->snprintf(text, sizeof(text), "%u:%02u",
                              (unsigned int)(seconds / 60U),
                              (unsigned int)(seconds % 60U));
            wo.ui->label_set_text(wo.target_label, text);
            wo.ui->label_set_text(wo.info_label,
                L("Maintiens la position", "Hold the position", "Mantén la posición", "Mantieni la posizione", "Halte die Position", "保持姿势"));
        } else {
            wo.libc->snprintf(text, sizeof(text), "%u", (unsigned int)ex->target);
            wo.ui->label_set_text(wo.target_label, text);
            wo.ui->label_set_text(wo.info_label,
                L("repetitions", "repetitions", "repeticiones", "ripetizioni", "Wiederholungen", "次数"));
        }
        wo.ui->label_set_text(wo.hint_label,
            L("Bouton : valider", "Button: done", "Botón: validar", "Pulsante: conferma", "Taste: erledigt", "按钮：确认"));
    }
}

static void send_done_event(void)
{
    uint8_t payload[7];
    uint32_t seconds = (wo.session_elapsed_ms + 999U) / 1000U;
    payload[0] = PROTOCOL_VERSION;
    payload[1] = EVENT_WORKOUT_DONE;
    payload[2] = (uint8_t)(wo.sets_done & 0xFFU);
    payload[3] = (uint8_t)(wo.sets_done >> 8);
    payload[4] = (uint8_t)(seconds & 0xFFU);
    payload[5] = (uint8_t)((seconds >> 8) & 0xFFU);
    payload[6] = 0U;
    (void)wo.host->bt_send(EVENT_CHANNEL, payload, sizeof(payload));
}

static void reset_session(void)
{
    wo.ex_index = 0U;
    wo.set_index = 1U;
    wo.resting = false;
    wo.running = false;
    wo.paused = false;
    wo.done = false;
    wo.phase_remaining_ms = 0U;
    wo.session_elapsed_ms = 0U;
    wo.sets_done = 0U;
}

/* Begins the current set: a countdown for TIME mode, nothing for REPS. */
static void begin_set(void)
{
    const workout_ex_t *ex = &wo.exercises[wo.ex_index];
    wo.resting = false;
    if (ex->mode == MODE_TIME)
        wo.phase_remaining_ms = (uint32_t)ex->target * 1000U;
    else
        wo.phase_remaining_ms = 0U;
}

static void start_first_set(void)
{
    wo.ex_index = 0U;
    wo.set_index = 1U;
    wo.sets_done = 0U;
    wo.session_elapsed_ms = 0U;
    wo.done = false;
    wo.paused = false;
    wo.running = true;
    begin_set();
    refresh_display();
}

static void finish_workout(void)
{
    wo.running = false;
    wo.paused = false;
    wo.resting = false;
    wo.done = true;
    send_done_event();
    refresh_display();
}

/* Called when the current set is validated (button or timer). Moves to the
 * next set, a rest, the next exercise, or the end of the workout. */
static void complete_set(void)
{
    const workout_ex_t *ex = &wo.exercises[wo.ex_index];

    if (wo.sets_done < 0xFFFFU) wo.sets_done++;

    if (wo.set_index < ex->sets) {
        wo.set_index++;
        if (ex->rest_sec > 0U) {
            wo.resting = true;
            wo.phase_remaining_ms = (uint32_t)ex->rest_sec * 1000U;
        } else {
            begin_set();
        }
    } else if ((uint8_t)(wo.ex_index + 1U) < wo.count) {
        wo.ex_index++;
        wo.set_index = 1U;
        if (ex->rest_sec > 0U) {
            wo.resting = true;
            wo.phase_remaining_ms = (uint32_t)ex->rest_sec * 1000U;
        } else {
            begin_set();
        }
    } else {
        finish_workout();
        return;
    }
    refresh_display();
}

static void advance_time(uint32_t elapsed_ms)
{
    if (elapsed_ms >= wo.phase_remaining_ms) {
        wo.phase_remaining_ms = 0U;
        if (wo.resting) {
            begin_set();
            refresh_display();
        } else {
            complete_set();
        }
    } else {
        wo.phase_remaining_ms -= elapsed_ms;
    }
}

static bool parse_program(const uint8_t *data, uint32_t length)
{
    uint32_t offset = 3U;
    uint8_t count;
    uint8_t i;

    if (length < 3U) return false;
    count = data[2];
    if (count > MAX_EXERCISES) count = MAX_EXERCISES;

    for (i = 0U; i < count; i++) {
        uint8_t name_len;
        uint8_t mode;
        uint16_t target;
        uint8_t sets;
        uint16_t rest_sec;
        uint8_t copy;
        uint8_t j;

        if (offset + 1U > length) return false;
        name_len = data[offset++];
        if (offset + name_len > length) return false;
        copy = name_len > NAME_MAX ? NAME_MAX : name_len;
        for (j = 0U; j < copy; j++) wo.exercises[i].name[j] = (char)data[offset + j];
        wo.exercises[i].name[copy] = '\0';
        offset += name_len;

        if (offset + 5U > length) return false;
        mode = data[offset++];
        target = (uint16_t)(data[offset] | (data[offset + 1] << 8));
        offset += 2U;
        sets = data[offset++];
        rest_sec = (uint16_t)(data[offset] | (data[offset + 1] << 8));
        offset += 2U;

        wo.exercises[i].mode = mode == MODE_TIME ? MODE_TIME : MODE_REPS;
        wo.exercises[i].target = (uint16_t)clamp_i32(
            (int32_t)target, MIN_TARGET,
            wo.exercises[i].mode == MODE_TIME ? MAX_TIME_SEC : MAX_REPS);
        wo.exercises[i].sets = (uint8_t)clamp_i32((int32_t)sets, MIN_SETS, MAX_SETS);
        wo.exercises[i].rest_sec = (uint16_t)clamp_i32((int32_t)rest_sec, 0, MAX_REST_SEC);
    }

    wo.count = count;
    reset_session();
    return true;
}

static void handle_settings_message(const uint8_t *data, uint32_t length)
{
    if (data == 0 || length < 2U || data[0] != PROTOCOL_VERSION) return;

    switch (data[1]) {
    case COMMAND_PROGRAM:
        (void)parse_program(data, length);
        refresh_display();
        break;
    case COMMAND_START:
        if (wo.count > 0U) start_first_set();
        break;
    case COMMAND_PAUSE:
        if (wo.running) {
            wo.paused = !wo.paused;
            refresh_display();
        }
        break;
    case COMMAND_STOP:
        reset_session();
        refresh_display();
        break;
    case COMMAND_SKIP:
        if (wo.running && !wo.paused) {
            if (wo.resting) {
                begin_set();
                refresh_display();
            } else {
                complete_set();
            }
        }
        break;
    default:
        break;
    }
}

static gm_plugin_result_t workout_start(void *context)
{
    gm_plugin_display_info_t display;
    gm_plugin_lvgl_obj_t *root;
    char locale[GM_PLUGIN_LOCALE_TAG_MAX];
    (void)context;

    wo.language = 0U;
    if (wo.host->locale_get != 0 &&
        wo.host->locale_get(locale) == GM_PLUGIN_OK)
        wo.language = workout_detect_language(locale);

    if (wo.host->display_get_info(&display) != GM_PLUGIN_OK ||
        display.width <= SCREEN_MARGIN * 2U ||
        display.height < EX_NAME_HEIGHT + SERIES_HEIGHT + TARGET_HEIGHT + HINT_HEIGHT + 70)
        return GM_PLUGIN_ESTATE;

    root = wo.ui->root_get();
    if (root == 0) return GM_PLUGIN_ESTATE;
    wo.ui->obj_clean(root);

    wo.screen = wo.ui->obj_create(root);
    if (wo.screen == 0) return GM_PLUGIN_ENOMEM;
    wo.ui->obj_set_size(wo.screen, display.width, display.height);
    wo.ui->obj_align(wo.screen, GM_PLUGIN_LVGL_ALIGN_CENTER, 0, 0);
    style_panel(wo.screen, 0x00, 0, 0);

    wo.title_label = make_label(wo.screen, L("Muscu", "Workout", "Gimnasio", "Palestra", "Training", "健身"), 0x80);
    if (wo.title_label == 0) goto no_memory;
    wo.ui->obj_set_size(wo.title_label, display.width - SCREEN_MARGIN * 2U, 24);
    wo.ui->obj_align(wo.title_label, GM_PLUGIN_LVGL_ALIGN_TOP_MID, 0, 8);

    wo.ex_label = make_label(wo.screen, "", 0xFF);
    if (wo.ex_label == 0) goto no_memory;
    {
        gm_plugin_lvgl_style_value_t large_font = {0};
        large_font.ptr = wo.ui->font_large;
        set_style(wo.ex_label, GM_PLUGIN_LVGL_STYLE_TEXT_FONT, large_font);
    }
    wo.ui->obj_set_size(wo.ex_label, display.width - SCREEN_MARGIN * 2U, EX_NAME_HEIGHT);
    wo.ui->obj_align(wo.ex_label, GM_PLUGIN_LVGL_ALIGN_CENTER, 0, -44);

    wo.series_label = make_label(wo.screen, "", 0x90);
    if (wo.series_label == 0) goto no_memory;
    wo.ui->obj_set_size(wo.series_label, display.width - SCREEN_MARGIN * 2U, SERIES_HEIGHT);
    wo.ui->obj_align(wo.series_label, GM_PLUGIN_LVGL_ALIGN_CENTER, 0, -6);

    wo.target_label = make_label(wo.screen, "", 0xFF);
    if (wo.target_label == 0) goto no_memory;
    {
        gm_plugin_lvgl_style_value_t large_font = {0};
        large_font.ptr = wo.ui->font_large;
        set_style(wo.target_label, GM_PLUGIN_LVGL_STYLE_TEXT_FONT, large_font);
    }
    wo.ui->obj_set_size(wo.target_label, display.width - SCREEN_MARGIN * 2U, TARGET_HEIGHT);
    wo.ui->obj_align(wo.target_label, GM_PLUGIN_LVGL_ALIGN_CENTER, 0, 36);

    wo.info_label = make_label(wo.screen, "", 0x80);
    if (wo.info_label == 0) goto no_memory;
    wo.ui->obj_set_size(wo.info_label, display.width - SCREEN_MARGIN * 2U, 24);
    wo.ui->obj_align(wo.info_label, GM_PLUGIN_LVGL_ALIGN_CENTER, 0, 78);

    wo.hint_label = make_label(wo.screen, "", 0x70);
    if (wo.hint_label == 0) goto no_memory;
    wo.ui->obj_set_size(wo.hint_label, display.width - SCREEN_MARGIN * 2U, HINT_HEIGHT);
    wo.ui->obj_align(wo.hint_label, GM_PLUGIN_LVGL_ALIGN_BOTTOM_MID, 0, -8);

    wo.count = 0U;
    wo.refresh_elapsed_ms = REFRESH_INTERVAL_MS;
    reset_session();

    refresh_display();
    return GM_PLUGIN_OK;

no_memory:
    wo.ui->obj_clean(root);
    wo.screen = 0;
    wo.title_label = 0;
    wo.ex_label = 0;
    wo.series_label = 0;
    wo.target_label = 0;
    wo.info_label = 0;
    wo.hint_label = 0;
    return GM_PLUGIN_ENOMEM;
}

static void workout_loop(void *context, uint32_t elapsed_ms)
{
    (void)context;

    if (elapsed_ms > 500U) elapsed_ms = 500U;

    if (wo.running && !wo.paused) {
        wo.session_elapsed_ms += elapsed_ms;
        if (wo.resting || (wo.ex_index < wo.count &&
                           wo.exercises[wo.ex_index].mode == MODE_TIME))
            advance_time(elapsed_ms);
    }

    if (wo.refresh_elapsed_ms < REFRESH_INTERVAL_MS) {
        uint32_t remaining = REFRESH_INTERVAL_MS - wo.refresh_elapsed_ms;
        wo.refresh_elapsed_ms = elapsed_ms >= remaining
            ? REFRESH_INTERVAL_MS : wo.refresh_elapsed_ms + elapsed_ms;
    }

    if (wo.refresh_elapsed_ms >= REFRESH_INTERVAL_MS) {
        wo.refresh_elapsed_ms = 0U;
        if (wo.running && !wo.paused) refresh_display();
    }
}

static bool workout_event(void *context, const gm_plugin_event_t *event)
{
    (void)context;
    if (event == 0) return false;

    if (event->type == GM_PLUGIN_EVENT_BT_MESSAGE) {
        if (event->data.bt.channel != SETTINGS_CHANNEL || event->data.bt.data == 0)
            return false;
        handle_settings_message(event->data.bt.data, event->data.bt.length);
        return true;
    }

    if (event->type == GM_PLUGIN_EVENT_BUTTON &&
        event->data.button.button == GM_PLUGIN_BUTTON_PRIMARY) {
        if (event->data.button.action == GM_PLUGIN_BUTTON_ACTION_SINGLE) {
            if (!wo.running) {
                if (wo.count > 0U) start_first_set();
            } else if (wo.paused) {
                wo.paused = false;
                refresh_display();
            } else if (wo.resting) {
                begin_set();
                refresh_display();
            } else {
                complete_set();
            }
            return true;
        }
        if (event->data.button.action == GM_PLUGIN_BUTTON_ACTION_LONG) {
            reset_session();
            refresh_display();
            return true;
        }
    }
    return false;
}

static void workout_stop(void *context)
{
    gm_plugin_lvgl_obj_t *root;
    (void)context;
    root = wo.ui->root_get();
    if (root != 0) wo.ui->obj_clean(root);
    wo.screen = 0;
    wo.title_label = 0;
    wo.ex_label = 0;
    wo.series_label = 0;
    wo.target_label = 0;
    wo.info_label = 0;
    wo.hint_label = 0;
    wo.running = false;
    wo.paused = false;
}

gm_plugin_result_t gm_plugin_entry(const gm_plugin_host_api_t *host,
                                    gm_plugin_descriptor_t *plugin)
{
    const gm_plugin_capabilities_t required =
        GM_PLUGIN_CAP_BLUETOOTH | GM_PLUGIN_CAP_BUTTON;

    if (host == 0 || plugin == 0 || host->log == 0 || host->bt_send == 0 ||
        host->display_get_info == 0 || host->graphics.lvgl == 0 ||
        !GM_PLUGIN_VERSION_COMPATIBLE(host->abi_version,
                                      GM_PLUGIN_ABI_MIN_VERSION) ||
        host->struct_size < GM_PLUGIN_HOST_API_MIN_SIZE ||
        plugin->struct_size < GM_PLUGIN_DESCRIPTOR_MIN_SIZE ||
        (host->capabilities & required) != required)
        return GM_PLUGIN_ENOTSUP;

    wo.host = host;
    wo.ui = host->graphics.lvgl;
    if (gm_plugin_libc_get(host, &wo.libc) != GM_PLUGIN_OK)
        return GM_PLUGIN_ENOTSUP;
    if (wo.ui->struct_size < GM_PLUGIN_LVGL_API_MIN_SIZE ||
        !GM_PLUGIN_VERSION_COMPATIBLE(wo.ui->api_version,
                                      GM_PLUGIN_LVGL_API_MIN_VERSION))
        return GM_PLUGIN_EVERSION;

    plugin->abi_version = GM_PLUGIN_ABI_MIN_VERSION;
    plugin->context = &wo;
    plugin->on_start = workout_start;
    plugin->on_loop = workout_loop;
    plugin->on_event = workout_event;
    plugin->on_stop = workout_stop;
    return GM_PLUGIN_OK;
}
