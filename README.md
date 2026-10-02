# MemoMind — Séance de musculation guidée (workout)

Application **appairée** MemoMind : un coach de musculation qui déroule un programme d'exercices sur les lunettes, piloté et configuré depuis le téléphone.

## Composition

| Côté | Dossier | Artefact |
| --- | --- | --- |
| Lunettes (plugin natif C) | `glass/workout/` | `builds/workout.gmp` |
| Téléphone (plugin Web) | `phone/workout/` | `builds/workout-1.0.0.mmpkg` |

## Fonctionnement

- Le téléphone permet de composer une séance : liste d'exercices (nom, mode **répétitions** ou **durée**, cible, nombre de séries, temps de repos). Trois séances types sont fournies : **Corps entier**, **Haut du corps**, **Gainage**.
- Les lunettes affichent en grand l'exercice courant et la série, la cible (reps ou chrono `mm:ss`), puis un écran de repos avec décompte et « Suivant : ». Appui court sur le bouton pour valider / passer le repos, appui long pour réinitialiser.
- Le téléphone commande Démarrer / Pause / Arrêter et journalise les séances terminées (séances du jour, des 7 derniers jours, séries cumulées).
- Bilingue FR/EN côté lunettes (selon la locale de l'appareil).

## Protocole binaire

`SETTINGS_CHANNEL = 0x574b` ('WK', téléphone → lunettes), `EVENT_CHANNEL = 0x574c` ('WL', lunettes → téléphone), version 1.

- `COMMAND_PROGRAM` = `[version, cmd, count, (name_len, name..., mode, target_lo, target_hi, sets, rest_lo, rest_hi)*]`
- `COMMAND_START` / `COMMAND_PAUSE` / `COMMAND_STOP` / `COMMAND_SKIP`
- `EVENT_WORKOUT_DONE` = `[version, event, sets u16, seconds u16]`

## Build

Nécessite le SDK officiel MemoMind (`memomind-open/plugin-open-platform`). Déposer `glass/workout/` dans `GlassSDK/examples/` et `phone/workout/` dans `PhoneSDK/examples/`, puis lancer `python3 build.py`.

- Lunettes : `python3 build.py glass` → `.gmp`
- Téléphone : `python3 build.py web` → `.mmpkg`
