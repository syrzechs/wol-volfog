# patches/propios: los demás cambios propios de World of Legends en el cliente (GPL-3.0-only)

Además de la niebla volumétrica (los 18 parches de la carpeta `patches/`), `WowClient64.exe` del paquete **cliente64 v13 beta1** lleva
otros cambios propios del cliente reconstruido [wowdecomp](https://github.com/SatyPardus/wowdecomp). Aquí están como parches (`git diff`,
texto LF), con la **misma licencia que el resto del repositorio: GPL-3.0-only** (ver [LICENSE](../../LICENSE) y [NOTICE](../../NOTICE)).
Las líneas nuevas llevan el comentario «Not in the original client (World of Legends)»; los `// VERIFY:` / `// OFFSET:` de las funciones
originales siguen intactos. Como todo diff, cada parche lleva unas líneas de contexto alrededor de cada cambio: el cliente y sus datos
pertenecen a sus propietarios y no se incluyen.

## Qué es cada parche

| Parche | Bytes | Qué añade | CVars / comandos |
|---|---:|---|---|
| `00f-filelog.patch` | 7 371 | Base de `hd-cache`: registro de aperturas de fichero `Logs\files.log` (solo rutas `Tileset\`), en Storm/SFile y `Client_CVar.cpp`. Salió del diagnóstico del terreno y no tenía parche propio | `wolFileLog` 0/1 (o variable de entorno `WOL_FILELOG=1`) |
| `cliente64.patch` | 12 518 | Carpeta `Data\WoL64\*.MPQ` (la lee solo el cliente de 64 bits), versión visible `wolBuild` (también primera línea de `WowClient_crash.log` y `Logs\gx.log`) y bloque `VERSIONINFO` | `wolBuild` (solo lectura) |
| `hd-cache.patch` | 13 817 | Presupuesto de la caché de texturas configurable y registro de aperturas de `.blp` | `wolTextureCacheMB` 0 / 256-2048, `wolFileLog`; addon `/wolgfx hd on\|off` |
| `E1-post-fxaa-tonemap.patch` | 40 342 | Pase de post-proceso `WolPost`: antialiasing FXAA 3.11 (reescritura propia, ver la nota de licencia) y tonemapping filmic en luz lineal | `wolAA` 0/1/2, `wolTonemap` 0/1, `wolExposure`, `wolSaturation`; `/wolgfx post`, `aa`, `tonemap` |
| `E3-sombras-pcf.patch` | 10 817 | Bordes de sombra más suaves ensanchando los desplazamientos PCF por CPU, sin tocar shaders | `wolShadowSoft` 0-3; `/wolgfx sombras 0-3` |
| `E4-gradacion-zona.patch` | 36 014 | Gradación de color por zona y hora (bordes `DayNight`) dentro del pase de post de E1; sube `kMaxPermutations` a 12 | `wolGradeStrength` 0-1, `wolGradeDebug`; se aplica después de E1 |
| `E2-hbao.patch` | 60 996 | Oclusión ambiental HBAO a media resolución sobre el depth INTZ de la niebla. **EXPERIMENTAL**: estable, pero visualmente incorrecta (halos en bordes finos); no está en ningún perfil y viene apagada | `wolAO` 0/1/2, `wolAORadius`, `wolAOStrength`, `wolAODebug` |
| `E2b-hbao-correccion.patch` | 17 428 | Corrige E2 (se aplica **después** de `E2-hbao.patch`): tope de radio 48 -> 96 px con la caída del radio realmente muestreado, normal que mira a la cámara cuando el vecino de un eje es otra superficie, ruido Bayer 4x4 en vez de gradiente intercalado, composición otra vez con `GxBlend_Mod`, `wolAODebug` 0-4 (2 = profundidad lineal). Probado en Ventormenta (mapa de AO con contacto suelo-pared correcto); **sigue experimental** y apagado | `wolAODebug` 0-4 |

La documentación de cada pieza (diseño, pruebas, cifras, lo que no salió) está en el repositorio de trabajo en `patches-cliente/<pieza>/README-*.md`;
los resúmenes de arriba salen de ahí. Los shaders de `WolPost` y de `WolAO` se compilan con la misma cadena `fxc -> mkbls.py -> mkmpq.py` que los de
la niebla y acaban en el mismo `Data\WoL64\patch-Z.MPQ` del paquete.

## Orden de aplicación

`aplicar.ps1` aplica ahora **los 26 parches** (los 18 de `patches/` y los 8 de esta carpeta) sobre un `wowdecomp` limpio (`0e4e9fe`), en este orden, que es el
de su lista `$Orden` y el que se ha verificado:

1. `00a`…`00e` y `01-B1`…`13-B13b` (los 18 de `patches/`; `00e-sin-consola` es la «sin consola»: ya no hay un duplicado en esta carpeta)
2. `00f-filelog.patch` (base de `hd-cache`)
3. `E1-post-fxaa-tonemap.patch` (después de la niebla, B1-B13b)
4. `hd-cache.patch` (después de `00b-topes-2008`, `00c-sombras-4096` y `00f-filelog`)
5. `E3-sombras-pcf.patch` (toca los mismos `WorldParam.h` y `WorldParam_CVar.cpp` que `hd-cache`)
6. `E2-hbao.patch` (después de E1: reutiliza su pase y el depth de la niebla)
7. `E2b-hbao-correccion.patch` (después de E2: corrige el mapa de AO)
8. `E4-gradacion-zona.patch` (después de E1, E2 y E2b)
9. `cliente64.patch` (el último: su contexto incluye `wolFileLog`)

```powershell
.\aplicar.ps1 -Destino C:\ruta\wowdecomp              # los 26 en orden; si uno falla, deshace los anteriores
.\aplicar.ps1 -Destino C:\ruta\wowdecomp -Revertir    # los deshace en orden inverso
```

### Verificado el 2026-10-21 sobre `git archive HEAD` de wowdecomp (commit `0e4e9fe`, directorio nuevo)

- Los 26 parches aplican en orden y `-Revertir` deja el árbol igual que `HEAD`.
- El árbol resultante es idéntico (`diff -r`, ignorando el final de línea CRLF/LF de los ficheros nuevos) al árbol de trabajo de World of Legends en `Engine/`, `WoW/`,
  `Shaders/`, `Tools/`, `CMakeLists.txt` y `waystone.rc` (salvo `build/`, `bin/` y `ThirdParty/coa-vfog-205ade5`).
- Regenerados el 2026-10-21 para que la cadena cierre: `00f-filelog.patch` (nuevo; el cambio del diagnóstico H4b nunca se había extraído), `hd-cache.patch` (ahora parte de
  `00f`: solo añade el registro de todos los `.blp`) y `cliente64.patch` (contexto de `Client_CVar.cpp` para aplicar después de `00f` y `hd-cache`). Los demás, sin cambios.

## Licencias de terceros de estos parches

- **FXAA (E1).** `wolAA` implementa el algoritmo «NVIDIA FXAA 3.11» de Timothy Lottes (NVIDIA). El HLSL de `WolPost` es una reescritura propia a partir de la
  descripción publicada del algoritmo y no contiene código de `Fxaa3_11.h` ni de otro port. El aviso de licencia de NVIDIA que lleva `Shaders/WolPost/NOTICE.txt`
  se **cotejó el 2026-10-21** palabra por palabra (ignorando espacios) con la cabecera pública de `Fxaa3_11.h` (copia íntegra del fichero original) y **coincide**; la frase
  de `NOTICE.txt` que dice que el texto «no se descargó» quedó obsoleta (el parche de E1 la conserva tal cual).
- **Tonemapping (E1).** Ajuste ACES de Krzysztof Narkowicz (fórmula publicada) y curvas sRGB de la norma IEC 61966-2-1. Sin código de terceros.
- El resto es de World of Legends bajo GPL-3.0-only.

## Avisos

- `cambios-cliente.diff` y estos parches contienen contexto del código reconstruido del cliente (igual que los 18 de `patches/`). Publicarlos es una decisión
  de los responsables del proyecto, no un requisito de la GPL para todos ellos; `WowClient64.exe` ya lleva todo esto dentro y por eso el paquete ofrece el código.
- `00f-filelog.patch` es la versión que solo registra `Tileset\`; `hd-cache.patch` la amplía a todos los `.blp`. Aplícalos en ese orden.
