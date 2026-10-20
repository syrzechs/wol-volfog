# wol-volfog: niebla volumétrica y haces de luz nativos para `WowClient64.exe`

Módulo de **World of Legends** que añade, dentro del cliente 3.3.5a reconstruido de [wowdecomp](https://github.com/SatyPardus/wowdecomp),
un pase de post-proceso con **niebla volumétrica** (tres capas: bruma, niebla baja y niebla de distancia), **luces puntuales
dispersas en la niebla** y **haces de luz del sol o la luna**. Se activa con la CVar `wolVolFog` (0 = cliente original, valor por
defecto; con 0 la imagen es la del cliente sin el módulo). Licencia: **GPL-3.0-only** (ver [LICENSE](LICENSE) y [NOTICE](NOTICE)).

*English summary at the end of this file.*

## Origen, autores y licencia

- Los shaders (16 ficheros `vf_*.hlsl`/`.hlsli`), el modelo de niebla, el estabilizador de luces y el orden de pases salen de
  **CoAVolFog (`coa-vfog`)**, congelado en el commit **`205ade5abe54524ccd2f449fb628903bc0b66be0`** (2026-10-19 13:38 UTC) de la rama
  `st0ny-main` de <https://github.com/Raz0r1337/coa-vfog>, que a su vez parte de <https://github.com/jealous-sound/coa-vfog>.
  Copyright de sus autores: **jealous-sound** y los colaboradores del fork de **Raz0r1337 (st0ny)**.
- CoAVolFog es **GPL-3.0-only** y su README añade un **permiso adicional bajo la sección 7 de la GPLv3**, que se conserva aquí tal cual:

  > As an additional permission under GPLv3 section 7, you may link or combine CoAVolFog, including modified versions, with the
  > World of Warcraft client, and distribute the resulting combination without providing the client's source code. GPLv3
  > continues to apply to CoAVolFog.

  En castellano: se puede enlazar o combinar CoAVolFog (también modificado) con el cliente de World of Warcraft y repartir la
  combinación sin el código del cliente; la GPLv3 sigue aplicándose a CoAVolFog. A quien reciba un `WowClient64.exe` con este
  módulo hay que ofrecerle **el código de este módulo** (este repositorio).
- Lo que hemos escrito nosotros (capa de formatos de Gx, textura de profundidad INTZ, volcado de depuración, lectura de los datos
  del cliente, empaquetado en `.bls`/MPQ, el shader de depuración `WolVolFogDebug.hlsl`) va bajo la misma licencia. Cada fichero
  derivado lleva su nota de modificación con fecha (GPLv3 §5a); el detalle por brief está en `Shaders/VolFog/NOTICE-modificaciones.txt`
  y el resumen en [NOTICE](NOTICE).
- **No se copia nada del cliente de Blizzard ni del árbol de wowdecomp**: solo ficheros nuestros y diffs. Los diffs (`patches/`,
  `cambios-cliente.diff`) llevan, como todo diff, unas líneas de contexto alrededor de cada cambio; el cliente y sus datos
  pertenecen a sus propietarios y no se incluyen.

## Qué contiene

```
LICENSE, NOTICE, README.md, aplicar.ps1, .gitattributes
patches/                      18 parches en el orden de aplicación (00a-00e prerrequisitos WoL, 01-B1 ... 12-B13, 13-B13b)
cambios-cliente.diff          lo que los parches cambian FUERA del módulo (16 ficheros), en un solo diff
Engine/Source/VolFog/         el módulo C++ (pase de niebla, modelo, luces, haces, entradas, volcado, textura INTZ)
Shaders/VolFog/               los HLSL, el CMake de la cadena fxc -> .bls -> MPQ, LICENSE y NOTICE-modificaciones.txt
Tools/VolFog/                 mkbls.py (bytecode -> contenedor GXSH .bls), mkmpq.py (escritor mínimo de MPQ)
WoW/Source/WorldClient/       VolFogWorld.{h,cpp}: lee sol, luces y niebla del cliente (DayNight, escena M2) y registra wolVolFogDump
```

`cambios-cliente.diff` es lo que un tercero necesita para **rehacer la integración**: las tablas de formatos de Gx (INTZ,
`A16B16G16R16F`, `A32B32G32R32F`), la llamada al pase desde `WorldFrame.cpp` (`OnWorldRender`), el empuje de la niebla lineal fuera de
rango en `DayNight::UpdateFog` y el registro de las CVars (`WorldParam*.{h,cpp}`, `ConsoleDevice_CVar.cpp`). Las líneas nuevas llevan
el comentario «Not in the original client (World of Legends)».

## Cómo se compila

Requisitos: Windows, Visual Studio 2022 (x64), CMake, Git, **Windows SDK** (para `fxc.exe`) y **Python 3** (solo biblioteca estándar).

1. Clona wowdecomp. Los parches se hicieron sobre el commit `0e4e9fe0a4a9fa08027c5eed4b2c5e873739cfd3` (2026-10-15) **más otros
   parches locales nuestros** del cliente (CVars `wolFarclipFactor`, `wolShadowMapSize`, `wolDpiAware`...). Consecuencia: los 11
   parches aplican limpios sobre ese árbol, pero sobre un wowdecomp sin tocar **43 de los 48 hunks de `cambios-cliente.diff` aplican**
   y 5 hay que colocarlos a mano (los de registro de CVars, que se apoyan en esas líneas: `WorldParam.h` 2, `WorldParam_CVar.cpp` 2,
   `ConsoleDevice_CVar.cpp` 1). Ponlos junto a las líneas equivalentes de tu árbol. La secuencia de los 11 parches, en cambio, se detiene en
   `02-B2` sobre un wowdecomp sin tocar (y se deshace sola): en ese caso usa `-Consolidado`.
2. Aplica el módulo, de una de las dos formas:
   ```powershell
   .\aplicar.ps1 -Destino C:\ruta\wowdecomp                 # los 11 parches en orden (git apply); si uno falla, deshace los anteriores
   .\aplicar.ps1 -Destino C:\ruta\wowdecomp -Consolidado    # copia el módulo y aplica cambios-cliente.diff con --reject (para un árbol distinto)
   .\aplicar.ps1 -Destino C:\ruta\wowdecomp -Revertir       # deshace los 11 en orden inverso (git apply -R)
   ```
   Orden de los parches: `B1` formatos Gx, `B2` shaders y pase base, `B3` depth INTZ, `B3b` correcciones, `B4B5` CVars y modelo, `B6`
   marcha y composición, `B7` filtro temporal, `B6b` correcciones, `B8` luces, `B9` haces, `B10` interiores y ajustes, `B13` MSAA con la niebla (el depth legible pasa a ser una copia RESZ del depth multimuestra; solo con DXVK, en D3D9 nativo de AMD el RESZ bloquea la GPU y se vuelve a MSAA 1 con aviso), `B13b` correcciones de B13 (el RESZ solo se ofrece si el `d3d9.dll` que usa el cliente es DXVK, reconocido por su contenido y no por su carpeta: un proxy como ReShade delante del driver nativo también se rechaza; el self-test distingue un depth erróneo de una llamada fallida). Los parches están
   en LF (lo que da `git diff` con `core.autocrlf=true`); el script reintenta con `--ignore-whitespace` si tu árbol está en CRLF sin `autocrlf`.
3. Compila:
   ```powershell
   cmake -S . -B build/x64 -G "Visual Studio 17 2022" -A x64 -DWOWDECOMP_DISABLE_SIGNATURE_CHECKS=ON
   cmake --build build/x64 --config Release --target WowClient --parallel
   ```
   La opción **`WOWDECOMP_VOLFOG`** (ON por defecto) compila los shaders con `fxc /O3` (ps_3_0 y vs_3_0, 22 permutaciones en 9 `.bls`), los empaqueta
   con `mkbls.py` y deja **`bin\Win\Data\patch-Z.MPQ`**. Con `-DWOWDECOMP_VOLFOG=OFF` no hace falta `fxc` ni Python y el cliente no
   tiene shaders de niebla. `-DVOLFOG_FXC=<ruta a fxc.exe>` y `-DVOLFOG_PYTHON=<python.exe>` fuerzan las rutas.
4. Copia `WowClient64.exe` y `patch-Z.MPQ` (junto a `Data\patch-3.MPQ`; en un cliente empaquetado, donde el cliente 64 lea sus MPQ propios).
   El cliente x64 se probó con Direct3D 9 nativo (AMD), con **DXVK** y con **D3D9Ex**. `WowClient32.exe` no se ha podido compilar aquí
   (falta NASM para FMOD), así que Win32 está sin probar; el backend OpenGL es un stub y solo se ha revisado en el código.

### Prerrequisitos

La secuencia `B1…B10` se escribió sobre un `wowdecomp` que ya llevaba los parches previos de World of Legends (`recordar-contrasena`, `topes-2008`, `sombras-4096`, `ui-depth`, `sin-consola`: CVars `wol*`, mapa de sombras, profundidad de la interfaz, sin consola de depuración), y varios hunks dependen de ellos. Van incluidos en `patches/00a…00e` y `aplicar.ps1` los aplica primero; sobre un `wowdecomp` limpio la secuencia completa son 17 parches. Esos cinco parches son código de World of Legends y se publican aquí bajo la misma licencia.

## Uso

Arranca con `SET wolVolFog "2"` en `Config.wtf` (el depth legible se crea al arrancar) o cambia en caliente y ejecuta `/console gxRestart`.
Pasar de 0 a >0 (o al revés) pide reiniciar los gráficos; entre 1, 2 y 3 el cambio es inmediato.

| CVar | Rango | Defecto | Guardada | Efecto |
|---|---|---|---|---|
| `wolVolFog` | 0-3 | 0 | sí | 0 = cliente original; 1 = cuarto de resolución, 16 pasos; 2 = media, 24; 3 = media, 32 |
| `wolVolFogDensity` | 0,0-4,0 | 1,0 | sí | multiplica bruma, niebla baja y niebla de distancia |
| `wolVolFogGodRays` | 0,0-1,0 | 0,0 | sí | fuerza de los haces de luz (0 = apagados) |
| `wolVolFogLocalLights` | 0/1 | 1 | sí | luces puntuales dispersas en la niebla (solo con `wolVolFog` 2 o 3) |
| `wolVolFogDebug` | 0-6 | 0 | sí (flag 1) | 1 profundidad, 2 búfer de marcha, 3 historia, 4 imagen sin filtro temporal, 5 rejilla, 6 textura de haces |

`/console wolVolFogDump` imprime en la consola (y añade a `Logs\volfog.log`) las entradas del modelo, las capas, las luces elegidas y
los haces. Con el addon `WoL_Graficos` 1.2: `/wolgfx niebla off|baja|media|alta`, `/wolgfx niebla rayos 0..1`, `/wolgfx niebla luces on|off`.
Con la niebla activa el MSAA se ignora (el depth se lee como textura INTZ); el perfil «ultra» del addon lo deja en 1.

## Limitaciones conocidas

- **Pérdida de dispositivo en pantalla completa**: solo comprobada a medias (dos pérdidas de foco de 5 s sin cierre ni errores); falta
  repetirla a mano con alt-tab real y comprobar que la niebla vuelve a dibujarse.
- **Win32** sin compilar (NASM) y **OpenGL** sin ejecutar; **D3D9Ex** probado sin luces puntuales.
- **Ocaso, amanecer y día** sin probar en juego al cerrar este paquete (pruebas hechas de noche); el glow con valor alto tampoco.
- Pocas **luces puntuales** reales: la mayoría de farolas del juego no son luces `M2LIGHT_POINT` (su brillo es de textura), así que el efecto
  de `wolVolFogLocalLights` es tenue. El `LightStay` de CoAVolFog (mantener la luz de un modelo que no se dibuja) no se ha portado.
- La variante con luces cuesta ~1,4 ms por fotograma en la prueba de Ventormenta; sin luces el coste es casi nulo en escenas limitadas por CPU.
- El ruido de densidad es un atlas 2D de 32 cortes (el volumen 3D original no existe en la capa Gx): el salto del eje y es un escalón.
- Sobre el cielo cercano al horizonte hay bruma (es el modelo de CoAVolFog; ver la nota de B6 en el informe del proyecto).

## English summary

**wol-volfog** adds native volumetric fog, scattered local lights and sun/moon light shafts to the 64-bit 3.3.5a client rebuilt in
[wowdecomp](https://github.com/SatyPardus/wowdecomp). It is a port of **CoAVolFog (`coa-vfog`)**, frozen at commit `205ade5` of
<https://github.com/Raz0r1337/coa-vfog> (`st0ny-main`, based on <https://github.com/jealous-sound/coa-vfog>), whose shaders, fog model, light
stabiliser and pass order are reused (derived mode: the data comes from the client's own `Light.dbc`/`DayNight`, not from memory
hooks). Authors: jealous-sound and the contributors of the Raz0r1337 fork. **License: GPL-3.0-only**, with the additional permission under
GPLv3 section 7 quoted above: CoAVolFog, including modified versions, may be linked or combined with the World of Warcraft client and the
combination distributed without the client's source code; GPLv3 keeps applying to CoAVolFog. Nothing from the Blizzard client or the whole
wowdecomp tree is copied here: only our files and diffs.

Build: wowdecomp (patches made on `0e4e9fe` plus our other local client patches) + `aplicar.ps1` (the patches in order, `-Consolidado` for the
module plus `cambios-cliente.diff`, `-Revertir`) + CMake option `WOWDECOMP_VOLFOG` (needs `fxc.exe` from the Windows SDK and Python 3;
it writes `bin\Win\Data\patch-Z.MPQ`). Use: `SET wolVolFog "2"` (0 off, 1-3 quality) and `gxRestart` when switching between 0 and >0;
`wolVolFogDensity`, `wolVolFogGodRays`, `wolVolFogLocalLights`, `wolVolFogDebug`, console command `wolVolFogDump`. Tested on an AMD RX 9070 XT
with native D3D9, DXVK and D3D9Ex (x64 only). Known limits: device loss in exclusive full screen only partly verified, Win32 not built (NASM for
FMOD), OpenGL stub, sunset/sunrise/day not tested, few real point lights, CoAVolFog's `LightStay` not ported. MSAA is ignored while the fog is on.
