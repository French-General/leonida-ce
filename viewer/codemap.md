# viewer/

## Responsibility

Visor/debug local de assets del mundo: inspección visual de `world.json` + chunks `.glb` en espacio mundo (Y-up, metros), con freecam. Es herramienta de desarrollo, no parte del juego ni del build principal.

Ficheros: `serve.py` (servidor local), `index.html` (visor Three.js monofichero).

## Design

- **Stack:** `http.server.ThreadedHTTPServer` (solo `127.0.0.1`, puerto `8765` por defecto) + frontend estático sin build (Three.js `0.160.0` por CDN vía importmap + `GLTFLoader`).
- **Backend (`serve.py`, ~47 líneas):** subclase de `SimpleHTTPRequestHandler` con `directory=HERE`. Traduce `/world/*` al directorio `--world` (por defecto `<work>\world`); resto se sirve desde `viewer/`. Incluye guarda anti-traversal (`..` → `__denied__`), `Cache-Control: no-store` y MIME `model/gltf-binary` para `.glb`. Se lanza con `python -I serve.py [--world DIR] [--port 8765]`; si `--world` no es directorio, `Handler.world = None` y el visor cae a terreno de prueba.
- **Frontend (`index.html`, ~213 líneas, sin dependencias locales):** escena Three.js (`WebGLRenderer` antialias, `HemisphereLight` + `DirectionalLight`, `Fog`, fondo cielo), freecam WASD + `Q`/`E` bajar/subir + `Shift` rápido + `Ctrl` lento + rueda = velocidad, `F` wireframe, `G` bounds de chunks (`Box3Helper`), `R` volver a `spawn`, `Esc` liberar ratón. Pointer-lock con fallback a drag-to-look para iframes/previews. HUD (posición, velocidad, fps, chunks cargados, draw calls, tris) + banner de aviso + ayuda de teclas.
- **Qué visualiza:** `world/world.json` → `{ spawn: [x,y,z], chunks: [{ id, url, bounds: [min-koodi,max-koodi] }] }` (fetch `no-store`); cada chunk es un `.glb` ya en coordenadas de mundo. Sin `world.json` (o con `?test`) genera terreno TEST procedural en navegador: `PlaneGeometry` 256 m por chunk con `hash/noise/height` determinista + color por altura + `InstancedMesh` de cajas; el banner lo indica explícitamente.
- **Streaming:** radios `LOAD_R 1400` / `UNLOAD_R 2000`, `MAX_INFLIGHT 4`, orden por distancia a `bounds`, `drop()` con `dispose()` de geometría/materiales. Sin LOD ni paginación más allá de carga/descarga por distancia.

## Flow

1. `python -I serve.py --world <dir>` → `http://127.0.0.1:8765/`; `/world/` mapea al mundo real.
2. `index.html` → `loadWorld()`: intenta `fetch("world/world.json")`, si falla → `testWorld()`.
3. `camera.position ← world.spawn`; precalcula `Box3Helper` por chunk (ocultos salvo `G`).
4. Bucle (`setAnimationLoop`): actualiza freecam (yaw/pitch + velocidad) → `stream()` (descarga lejanos, carga cercanos hasta `MAX_INFLIGHT`, test = `buildTestChunk()` síncrono, real = `GLTFLoader.load()` async) → `render()` + HUD.

## Integration

- **Sin dependencia del build principal:** no importa código del juego ni decodifica nada; consume artefactos ya generados (`world.json` + `.glb`) o terreno sintético. Los ficheros del juego nunca viven en este repo: `--world` los monta bajo `/world/` en tiempo de servicio.
- **Acoplamiento:** solo el contrato `world.json` (`spawn`, `chunks[].{id,url,bounds}`) y chunks `.glb` en espacio mundo. Cambios de formato de mundo o de sistema de coordenadas rompen el visor sin que el servidor lo detecte.
- **Límites:** requiere internet (CDN Three.js), `file://` no sirve para `world/` real (hace falta `serve.py`), sin auth/CORS más allá de localhost.
