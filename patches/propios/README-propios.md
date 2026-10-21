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
| `cliente64.patch` | 12 307 | Carpeta `Data\WoL64\*.MPQ` (la lee solo el cliente de 64 bits), versión visible `wolBuild` (también primera línea de `WowClient_crash.log` y `Logs\gx.log`) y bloque `VERSIONINFO` | `wolBuild` (solo lectura) |
| `hd-cache.patch` | 13 928 | Presupuesto de la caché de texturas configurable y registro de aperturas de `.blp` | `wolTextureCacheMB` 0 / 256-2048, `wolFileLog`; addon `/wolgfx hd on\|off` |
| `E1-post-fxaa-tonemap.patch` | 40 342 | Pase de post-proceso `WolPost`: antialiasing FXAA 3.11 (reescritura propia, ver la nota de licencia) y tonemapping filmic en luz lineal | `wolAA` 0/1/2, `wolTonemap` 0/1, `wolExposure`, `wolSaturation`; `/wolgfx post`, `aa`, `tonemap` |
| `E3-sombras-pcf.patch` | 10 817 | Bordes de sombra más suaves ensanchando los desplazamientos PCF por CPU, sin tocar shaders | `wolShadowSoft` 0-3; `/wolgfx sombras 0-3` |
| `E2-hbao.patch` | 60 996 | Oclusión ambiental HBAO a media resolución sobre el depth INTZ de la niebla. **EXPERIMENTAL**: estable, pero visualmente incorrecta (halos en bordes finos); no está en ningún perfil y viene apagada | `wolAO` 0/1/2, `wolAORadius`, `wolAOStrength`, `wolAODebug` |
| `sin-consola.patch` | 1 046 | Quita la consola de tildes. **Es el mismo fichero que `patches/00e-sin-consola.patch`** (SHA-256 `331b4ca4…`): ya está entre los 18; no lo apliques otra vez | — |

La documentación de cada pieza (diseño, pruebas, cifras, lo que no salió) está en el repositorio de trabajo en `patches-cliente/<pieza>/README-*.md`;
los resúmenes de arriba salen de ahí. Los shaders de `WolPost` y de `WolAO` se compilan con la misma cadena `fxc -> mkbls.py -> mkmpq.py` que los de
la niebla y acaban en el mismo `Data\WoL64\patch-Z.MPQ` del paquete.

## Orden de aplicación

Sobre un wowdecomp con los 18 parches de `patches/` ya aplicados (`aplicar.ps1`), en este orden (es el orden en que se hicieron; los READMEs de
cada pieza dicen de cuáles dependen):

1. `cliente64.patch` (va después de `00a-recordar-contrasena`, `00e-sin-consola` y la niebla)
2. `hd-cache.patch` (después de `00b-topes-2008` y `00c-sombras-4096`)
3. `E1-post-fxaa-tonemap.patch` (después de la niebla, B1-B13b)
4. `E3-sombras-pcf.patch` (después de `00c-sombras-4096`; toca los mismos `WorldParam.h` y `WorldParam_CVar.cpp`)
5. `E2-hbao.patch` (después de E1: reutiliza su pase y el depth de la niebla)

```powershell
cd C:\ruta\wowdecomp
foreach ($p in 'cliente64','hd-cache','E1-post-fxaa-tonemap','E3-sombras-pcf','E2-hbao') { git apply "C:\ruta\wol-volfog\patches\propios\$p.patch"; if ($LASTEXITCODE) { "falla $p"; break } }
```

`aplicar.ps1` **no** aplica esta carpeta: solo la lista de `patches/`.

### Comprobado el 2026-10-21 sobre `git archive HEAD` de wowdecomp (commit `0e4e9fe`, sin tocar nada más)

- Los 18 parches de `patches/` y después `cliente64.patch`: **aplican** (19 de 19).
- `E1-post-fxaa-tonemap.patch` y después `E2-hbao.patch`: **aplican** sobre eso.
- **`hd-cache.patch` NO aplica sobre un árbol limpio**: falla en `SFile.cpp`, `SFile.h` y `Client_CVar.cpp`. Presupone una versión anterior de `wolFileLog` (solo
  registraba `Tileset\`) que existía en el árbol de trabajo sin parche propio. **`E3-sombras-pcf.patch` tampoco aplica sin `hd-cache.patch`** (su contexto en
  `WorldParam.h` incluye la CVar `wolTextureCacheMB`). Para cerrarlo falta un parche base de `wolFileLog` (o regenerar `hd-cache.patch` contra HEAD + niebla +
  `cliente64`); no se hizo en esta pasada. Hasta entonces, el orden de arriba solo se reproduce completo en el árbol de trabajo de World of Legends.

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
- `hd-cache.patch` incluye la parte de `wolFileLog` que registra todos los `.blp` (en el árbol de trabajo existía antes una versión que solo registraba `Tileset\`,
  sin parche propio); si `git apply` falla en `SFile.cpp` o `Client_CVar.cpp`, ese es el punto a mirar.

> Nota 21-oct: `hd-cache.patch` y `E3-sombras-pcf.patch` dependen de un cambio previo del registro de aperturas de fichero (CVar `wolFileLog`, hecho durante el diagnóstico H4b) que aún no está extraído como parche; se añadirá como `00f-filelog.patch`.
