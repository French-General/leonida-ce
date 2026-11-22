# game/

## Responsibility

Fuente de verdad del version pinning: `xbox-1.0.2.1.json` describe la única copia Xbox Series soportada (`title: "Xbox Series package"`, `version: "1.0.2.1"`). Contiene 108 entradas en `files` con `{size, sha256}` por ruta relativa, más `expected_exe: "game_scarlett_master.exe"`. El stage `check` de `make_game.py` lo usa como contrato de entrada: nada posterior (shaders, assets, runtime, pipelines) corre sin pasar esta puerta.

## Design

- Manifiesto por versión: un fichero `xbox-<version>.json` por versión soportada; `versions()` en `make_game.py` carga todos los `*.json` de `game/`. Añadir soporte a una versión nueva es añadir un JSON, sin tocar lógica.
- Doble nivel de verificación: `size` (barato, siempre) + `sha256` (caro, solo con `--full-check` vía `tools/manifest.py verify`). El JSON guarda ambos por fichero, desde DLLs pequeñas (`MicrosoftGame.Config`: 2542 B) hasta RPFs gigantes (`audio.rpf`: ~28 GB, `anim.rpf`: ~4.4 GB, `data.rpf`, `common.rpf`, `textures.rpf`, `packs.rpf`, `levels.rpf`, `shaders.rpf` y `data/ttdb/**/*.ttpf` troceados).
- Probes para identificación rápida: `PROBES = ("MicrosoftGame.Config", "Catalog000.bin", "appdata0.rpf")` — tres ficheros pequeños (2.5 KB / 815 KB / 8.8 KB) cuya existencia + tamaño bastan para reconocer la versión sin leer nada grande. Definidos en `make_game.py:91`, filtrados por `p in v["files"]` para tolerar manifiestos que no los tengan.
- `expected_exe` como aviso, no como fallo: `game_scarlett_master.exe` se comprueba al final de `check()`; si falta, el build sigue pero el stage `runtime` solo compila el loader sin poder ejecutarlo.

## Flow

1. `match_version(game)` → itera `versions()`; para cada manifiesto comprueba `os.path.isfile` + `os.path.getsize == v["files"][p]["size"]` solo para los probes. Devuelve el manifiesto coincidente o `None`. No abre ni hashea ningún fichero grande.
2. `load_version(game)` → envuelve `match_version`; si es `None` aborta con `fail()` listando `"<title> <version>"` soportados.
3. `find_dumps()` / `choose_game()` → rastrean Downloads, Desktop, Documents, Games, home y raíces de unidades (2 niveles) buscando carpetas con `MicrosoftGame.Config` y las validan con `match_version`. Es la autodetección de `--game`.
4. `check(game, full)` → `load_version` + bucle sobre las 108 entradas: `missing` / `wrong size`. Si hay errores, `fail()` pidiendo copia completa sin modificar de esa versión. Si `full=True`, delega el SHA-256 a `tools/manifest.py verify --manifest game/xbox-<version>.json`. Finalmente comprueba `expected_exe` y registra si el runtime será ejecutable.

## Integration

- Consumidor único: stage `check` de `make_game.py` (primero en `STAGES = ["check", "shaders", "assets", "runtime", "pipelines"]`). Su valor de retorno `v` (el dict del manifiesto) fija `v["version"]` y `v["files"]` para el resto del build.
- Productor de rutas: los stages `shaders`/`assets` asumen los RPFs listados aquí (`shaders.rpf`, `MODEL_ARCHIVES`, bancos AWC, pools de texturas); `runtime` asume `expected_exe`.
- Herramienta auxiliar: `tools/manifest.py` (`verify`) para la comprobación completa de hashes; el manifiesto nunca se edita en tiempo de build, solo se lee.
- Invariante: no descifrar ni parchear nada — `check` solo lee nombres, tamaños y (opcionalmente) hashes; `game/` no contiene datos del juego, solo el pin de la versión esperada.
