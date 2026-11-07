# MHGU para Android

Juega tu propia copia de Monster Hunter Generations Ultimate en Android, de forma nativa y con controles táctiles pensados
para teléfonos. No es un emulador: el código del juego se traduce para correr directamente en tu dispositivo.

**Este proyecto no incluye código del juego, archivos del juego, ROMs ni keys. Necesitas tu propia copia del juego.**

Hecho con Claude Code (Opus 5.5). Muchas de las decisiones fueron propias, porque la IA no está lista para tener la última palabra en algo así.

[Read this guide in English](README.md)

## Qué ofrece

### Cacería con controles táctiles

- **Movimiento:** un pad fijo, que no se mueve con el dedo. Arrastra más allá de su borde para correr; con el arma desenfundada eso se desactiva, para que no salgas corriendo en plena pelea
- **Cámara:** arrastra en cualquier parte de la pantalla para girarla, como en un juego de teléfono. Con un doble toque se centra
- **Fijar al monstruo:** toca el ícono del objetivo y la cámara lo sigue hasta que lo vuelvas a tocar, como en los Monster Hunter más nuevos. Solo sigue a un monstruo de tu misma zona. Se puede desactivar en el engranaje, **Controles**, **Fijar objetivo**: entonces el ícono gira la cámara una vez, como en el juego original
- **Layout propio para arcos y ballestas:** con uno en la mano, en una cacería, los controles cambian solos: un disparo grande bajo el pulgar, recarga, mira con un toque o apuntado rápido si lo mantienes, fuego agachado, y flechas para la munición. Si arrastras sobre el disparo mientras lo mantienes, también apuntas. Respeta la opción Bow Controls del juego y se acomoda aparte en el editor
- **Cambiar munición al deslizar:** además de las flechas, desliza hacia arriba o abajo sobre la barra de ítems. La lista del juego ahora se ve completa, con los nombres de cada munición
- **Disparos con el monstruo fijado:** salen hacia el centro de su cuerpo, también si está más alto o más bajo que tú. Para elevar el tiro, una ballesta entra un momento en apuntado rápido. Sin fijarlo, apuntas como siempre
- **Objetos:** la barra queda abajo al centro. Tócala para usar el objeto y deslízala para cambiarlo; un deslizamiento rápido ya no lo usa por error. También puedes activar flechas a sus lados: en el engranaje, **Controles**, **Botones en pantalla**, **Flechas de ítems**
- **Hunter Arts:** toca el panel de cada una para usarla
- **Ataques:** tres disposiciones a elegir: píldora dividida en diagonal, botones separados o una banda en U en la esquina de la pantalla
- **Otros botones:** esquivar, cubrirse y enfundar el arma, cada uno con su ícono

### Menús y comodidad

- **Menús pensados para el pulgar:** el pad de movimiento se queda en su lugar y mueve el cursor. Aceptar y Volver quedan en una píldora en el mismo lugar que los ataques, y arrastrar en la pantalla también mueve el cursor. L y R son pestañas
- El teclado del teléfono sirve para escribir el nombre de tu personaje
- El menú de opciones se usa con el dedo y tiene el estilo visual del juego
- **Beber y comer más rápido:** dos trucos opcionales, apagados por defecto, en el engranaje, **Trucos**. Aceleran la animación x1,7 o x2,5, pero no permiten caminar mientras bebes
- **Tus propios trucos y mods:** mira [Trucos y mods](#trucos-y-mods)
- Importa tu partida desde un emulador o desde otro dispositivo, y expórtala para llevarla a un emulador
- El juego se pausa solo cuando sales de la app
- Vibración al tocar los botones y vibración del juego en el teléfono, con o sin joystick

### Personalización

- Mantén apretado el engranaje para abrir el editor: mueve y cambia el tamaño de cada botón, y mueve la barra de objetos, las Hunter Arts y el ícono del objetivo
- La interfaz del juego se reacomoda para no quedar bajo los pulgares. Se puede desactivar
- Opacidad de los controles y velocidad de la cámara ajustables
- **Botones en pantalla:** en el engranaje, **Controles**, **Botones en pantalla**, elige cuáles se ven. Puedes sumar L, ZL, ZR, -, la cruceta y los clics de las palancas, que vienen ocultos. En el pueblo ZL aparece siempre, porque abre el menú multijugador. En los menús del juego también puedes sumar ZL y ZR (**ZL en los menús**, **ZR en los menús**), que algunos menús usan para cambiar de sección o ver más información
- **Botones combinados:** en esa misma pantalla, crea hasta 6 botones propios que aprietan varios a la vez, como A+X. Se ubican con el editor y también se pueden asignar a un botón del joystick

### Pantalla y rendimiento

- Hasta 120 fps en teléfonos potentes, con límite de FPS configurable
- Llena toda la pantalla, desde teléfonos ultra anchos hasta consolas portátiles 4:3
- Resolución 3D ajustable, hasta el 25 %, para ganar rendimiento en equipos modestos
- **Sombras:** apágalas para ganar velocidad, en el engranaje, **Gráficos y audio**, **Sombras**. En un Moto g06 bajó cerca de un 15 % el trabajo de la GPU por cuadro en una cacería
- **Modo gama baja:** para teléfonos débiles. Apaga el bloom, el desenfoque de profundidad y las sombras, usa texturas más livianas y mantiene el HUD nítido sobre una imagen 3D más chica
- **Modo tostadora:** la tercera opción del modo gama baja, para los teléfonos más débiles y para la batería: resolución 3D al 25 %, 30 fps, sin reverberación, y el HUD y las texturas todavía más livianos. En un Moto g06 se mantuvo en 30 fps y gastó cerca de un tercio menos de energía que el modo gama baja a 60 fps
- **Bloom y desenfoque de profundidad** y **Reverberación** (engranaje, **Gráficos y audio**): interruptores propios. Apagados, la GPU o el procesador trabajan menos
- Contador opcional de FPS y temperatura
- Usa menos memoria en dispositivos con GPU Mali y en teléfonos con poca RAM
- La pantalla sigue tu límite de FPS: a 60 FPS corre a 60 Hz aunque admita 120
- **Si tu teléfono calienta, baja el límite de FPS.** Es lo que más ayuda: de 120 a 60 consumió entre un cuarto y un tercio menos en un Galaxy S25 Ultra, y de 60 a 30 un quinto menos en un Moto g06
- En GPU Mali las texturas se decodifican en varios núcleos, para reducir los tirones
- **Driver de GPU propio:** solo en Adreno y bajo tu propio riesgo. En el engranaje, **Gráficos y audio**, **Driver de GPU**, agrega un .zip con `meta.json` y la librería. Por defecto se usa el del sistema, y si uno falla al arrancar o no carga, la app vuelve sola a ese y te avisa
- **Corrección de Adreno:** en una Adreno con driver Vulkan 1.1 y resolución 3D baja, la app deja MSAA 2x para que la imagen no se repita en grilla. Si se ve oscura o parpadea, o para ganar velocidad, ponla en **Off** en el engranaje, **Gráficos y audio**

### Con joystick

- Los controles táctiles se ocultan apenas conectas uno y vuelven cuando lo desconectas, sin reiniciar
- El engranaje queda siempre en pantalla, así que siempre puedes abrir las opciones. También puedes apretar **+** y **-** juntos
- El clic del stick derecho fija al monstruo
- **Cualquier joystick:** si el juego no lee bien el tuyo, entra al engranaje, **Controles**, **Mapear un mando**, y aprieta cada botón una vez. Queda guardado para ese modelo. Si un mando no se puede leer hasta mapearlo, la app te avisa al conectarlo

### Juego local

Dos dispositivos en el mismo Wi-Fi pueden jugar en la misma sala. En el pueblo, toca ZL y elige **Connect**. Uno entra a **Create Hub** y confirma con X; el otro entra a **Search Hubs** y elige la sala. Si no aparece, escribe la IP del anfitrión en el engranaje, **Red**, **Buscar servidores**. Se probó crear la sala, encontrarla y entrar en un Motorola y una Anbernic, pero todavía no una misión completa.

### Por internet

Cualquiera puede alojar un servidor de salas: el programa chico de la carpeta `server` (Go, un puerto para TCP y UDP, el 39510 por defecto; ver `server/README.md`). Todos los que quieran jugar juntos escriben el mismo servidor en el engranaje, **Red**, **Servidor de salas** (un nombre o una IP, y `:puerto` si no es el 39510); sus salas aparecen al buscar salas en el juego. Con una **Clave de emparejamiento**, solo los que tengan la misma clave ven sus salas. **Comprobar servidor de salas** dice si responde. Este proyecto no tiene un servidor propio, y el juego por internet todavía no se probó en Android.

Sin servidor: pon todos los dispositivos en la misma VPN (por ejemplo Tailscale o ZeroTier) y escribe la IP del anfitrión en esa VPN en **Buscar servidores**.

### Heredado de la versión de PC

La vida de los monstruos ajustable y las misiones de la aldea en el Hub también están en las opciones, pero todavía no se probaron a fondo en Android.

## Trucos y mods

- **Trucos:** archivos `.txt` en el formato de Atmosphere, para la versión 1.4.0 (su id de compilación es `FB08F1D20FD1204F`). En el engranaje, **Trucos**, **Agregar trucos**, elige el archivo y activa cada uno en esa misma página. Entran apagados.
- **Mods:** la carpeta de un mod con `romfs` o `exefs` adentro, o un `.zip` de esa carpeta. En el engranaje, **Mods**, **Agregar un mod**. Después se ordenan y se activan en esa página.
- También puedes copiarlos tú a la carpeta del juego, en carpetas `cheats` y `mods` junto a `exefs` y `romfs`. El juego los lee al arrancar.
- Los trucos y mods que cambian el código del juego funcionan, pero esa parte del juego corre más lenta. En línea, usa trucos solo si todos en la sala están de acuerdo.

## ¿Qué tan bien corre?

Medido con el contador en pantalla. En la columna del modo gama baja, la imagen 3D está al 40 % y el límite de FPS se subió, salvo en la sesión de 5 minutos, que usó el límite de 30 que pone el modo:

| Dispositivo | Chip | GPU | RAM | Normal | Modo gama baja |
|---|---|---|---|---|---|
| Galaxy S25 Ultra | Snapdragon 8 Elite | Adreno 830 | 12 GB | 120 fps | 120 fps en el pueblo |
| Anbernic RG406V | Unisoc T820 | Mali-G57 | 8 GB | 48 fps a resolución completa, 60 fps al 50 % | 60 fps |
| moto g06 power | MediaTek MT6769 | Mali-G52 MC2 | 4 GB | 16 fps a resolución completa, hasta 28 fps al 50 % | 45 fps; 30 fps estables en una sesión de 5 minutos |

En un teléfono con 4 GB de RAM, cierra las otras apps antes de jugar: Android puede cerrar el juego si se queda sin memoria.
Para que vaya más rápido, activa el modo gama baja: entra al engranaje, **Gráficos y audio**, **Modo gama baja**, guarda y reinicia el juego. Pone la resolución 3D al 40 % y el límite en 30 fps, y puedes cambiar los dos. Cuesta algo de nitidez: el juego se ve más suave.

## Qué necesitas

- **Un dispositivo Android:** Android 9 o superior (en Android 9 es experimental: no se pudo probar en uno), de 64 bits, con Vulkan 1.2. Casi cualquiera de los últimos años. Con Vulkan 1.1 funciona de forma experimental. En procesadores anteriores a 2018 la app adapta el código del juego la primera vez que arranca, unos segundos.
- **Unos 13 GB libres** en el dispositivo.
- **Una PC con Windows o Linux,** solo para la instalación, con unos 10 GB libres.
- **Tu propia copia del juego,** actualizada a la versión 1.4.0.

## Instalación

Son tres pasos, y se hacen una sola vez.

### Paso 1: Consigue los archivos de tu juego

Necesitas dos carpetas de tu propio juego: `exefs` y `romfs`. Deben incluir la actualización 1.4.0.

Si tu copia está en el emulador Ryujinx en tu PC:

1. Asegúrate de que el juego tenga aplicada la actualización 1.4.0.
2. Haz clic derecho en el juego, entra a **Extract Data** y extrae **ExeFS** y después **RomFS** en una carpeta vacía.

Deja las dos en una misma carpeta, por ejemplo `C:\MHGU`:

```
MHGU
├─ exefs
└─ romfs
```

Este proyecto no explica cómo volcar un juego de tu consola y no ofrece archivos del juego, keys ni enlaces a ellos.

### Paso 2: Crea el código del juego

La app no trae código del juego, así que creas el tuyo a partir de tu copia. Se hace en tu PC y no envía nada a ningún lado.

1. Instala [Python](https://www.python.org/downloads/). En Windows, marca **Add python.exe to PATH** en el instalador.
2. En esta página, haz clic en el botón verde **Code**, luego en **Download ZIP**, y descomprímelo.
3. Abre la carpeta descomprimida y entra a `tools`.
4. Windows: haz clic en la barra de direcciones de esa ventana, escribe `cmd` y presiona Enter. Linux: abre una terminal en esa carpeta.
5. Escribe esto, con tu propia carpeta, y presiona Enter:

   ```
   python build_game.py C:\MHGU
   ```

La primera vez descarga el compilador de Android de Google (unos 1,8 GB). Después trabaja solo: unos 6 minutos en una PC rápida,
más en una lenta. Cuando diga **Done**, tu carpeta tendrá dos archivos nuevos:

```
MHGU
├─ exefs
├─ romfs
├─ libmhgugame.so
└─ libmhgugame.id
```

**Estos dos archivos salen de tu juego: guárdalos para ti y no los compartas.**

### Paso 3: Instala

1. Copia la carpeta `MHGU` completa a tu dispositivo, por ejemplo con un cable USB. Puede ir en cualquier lugar de la memoria o de
   una tarjeta SD, menos dentro de `Android/data`.
2. En tu dispositivo, descarga el APK desde [Releases](../../releases/latest) y ábrelo. Android puede pedirte permitir la instalación
   de apps desde el navegador.
3. Abre **MHGU**, toca **Dar permiso y elegir carpeta** y activa el acceso a los archivos cuando Android lo pida. El juego solo lee esa carpeta.
4. Busca tu carpeta `MHGU` (aparece con un ✓) y toca **Usar esta carpeta**. La app tiene su propio explorador; si prefieres el de Android, toca **Selector de Android**.

La app revisa la carpeta, te dice exactamente qué falta si algo está mal y arranca el juego.

Tus partidas y ajustes se guardan aparte, en `Android/data/com.mhgu.recomp/files`. Para elegir otra carpeta después: engranaje,
**Opciones del menú**, **Carpeta del juego**.

## Importar tu partida

Puedes traer tu partida desde un emulador o desde otro dispositivo:

1. Busca el archivo `system` de tu partida. En Ryujinx: clic derecho en el juego, **Open User Save Directory**. En yuzu: clic derecho
   en el juego, **Open Save Data Location**.
2. Copia ese archivo a tu dispositivo, por ejemplo a Descargas.
3. En el juego, entra al engranaje, **Opciones del menú**, **Importar partida**, y elige el archivo.

Se reemplazan todos los personajes del dispositivo y el juego se reinicia. Tu partida anterior queda guardada en
`Android/data/com.mhgu.recomp/files/MHGURecomp/save_backups`.

## Exportar tu partida

Para llevar tu partida a un emulador o a otro dispositivo:

1. En el juego, guarda. Después entra al engranaje, **Opciones del menú**, **Exportar partida**, y elige una carpeta.
2. Ahí se crea una carpeta `MHGU_save_...` con los archivos `system` y `system_backup`. Pásala a tu PC.
3. En Ryujinx: clic derecho en el juego, **Open User Save Directory**. En yuzu: clic derecho en el juego, **Open Save Data Location**.
4. Guarda una copia de lo que haya ahí y copia los dos archivos.

## Samsung: Game Booster

El panel de rendimiento de Samsung solo aparece en apps que reconoce como juegos, y un APK instalado a mano no cuenta. Para
tenerlo, instala el APK una vez desde una PC con [adb](https://developer.android.com/tools/adb):

```
adb install -r -i com.mhgu.recomp MHGU-0.1.6.apk
```

Usa el nombre del APK que descargaste. Es opcional: el juego corre igual sin esto. Para al menos verlo en Gaming Hub: **Mis juegos**, **Biblioteca**, los tres
puntos, **Añadir juegos**.

## Si algo falla

| Problema | Qué hacer |
|---|---|
| `python` no se reconoce | Instala Python de nuevo, marca **Add python.exe to PATH** y abre otra terminal |
| "there is no exefs/main" | La ruta debe ser la carpeta que contiene `exefs` y `romfs` |
| "Este exefs no es de la versión 1.4.0" | Usa el `exefs` con la actualización 1.4.0 |
| "romfs está incompleto" | Usa el `romfs` del juego base con la actualización aplicada. Tiene 35.531 archivos |
| "Falta libmhgugame.so" | Haz el paso 2 y copia los dos archivos nuevos a la carpeta de tu dispositivo |
| "Este libmhgugame.so es de otra versión" | Una versión nueva de la app lo necesita otra vez: repite el paso 2 con la nueva descarga |
| El juego se cierra solo | Cierra otras apps y baja la Resolución 3D. Suele ser falta de memoria |
| Pantalla negra con solo los controles táctiles, o se cierra al presionar A | La copia de `romfs` a tu dispositivo no terminó. Cópiala de nuevo y revisa que ocupe 12,3 GB |
| Se cierra después de unos minutos de juego | Actualiza a la 0.1.6: usa la memoria de video de otra forma en GPU PowerVR. Si sigue pasando, comparte el registro: anota la memoria cada minuto |
| En un teléfono débil va lento | Activa el modo gama baja, apaga **Sombras** y prueba la resolución 3D al 33 % o al 25 %. Si sigue lento: modo gama baja, **Tostadora** |

Si sigue fallando, abre un [issue](../../issues) y adjunta el registro. Si el juego se cierra por un error, aparece una pantalla para
compartirlo; si no, está en el engranaje, **Opciones del menú**, **Compartir registro**. No adjuntes archivos del juego.

## Legal

Este proyecto no está afiliado, respaldado ni patrocinado por la Gran C ni la Gran N. Monster Hunter es una marca de la Gran C.
Switch es una marca de la Gran N.

Este repositorio y sus lanzamientos no contienen código del juego, datos del juego, ROMs ni keys. Úsalo solo con una copia del
juego que te pertenezca. Los pedidos de archivos del juego, keys o `libmhgugame.so` ya hechos se cerrarán.

Titulares de derechos: abran un issue para ponerse en contacto.

## Créditos

Basado en el proyecto de código abierto MHGURecomp, con [licencia MIT](LICENSE), y las bibliotecas listadas en
[THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES). Sus licencias también están en la app: engranaje, **Opciones del menú**, **Licencias**.

## Para desarrolladores

Mira [CLAUDE.md](CLAUDE.md) para ver cómo se organiza, se compila y se prueba el proyecto, con notas de instalación paso a paso.
