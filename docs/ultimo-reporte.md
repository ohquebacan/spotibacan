# CIA de prueba con el PR #4

`~/SpotiBacan-pr4.cia` (586 688 bytes), compilado del PR #4 tal cual
(commit `8cbfa7a`), sin el PR #2 ni tu rama local. Compiló sin errores con el
toolchain del proyecto, en un árbol aparte; tu rama
`fixes/lock-y-renovacion-login5` no se tocó.

El PR no cambia `cfg3ds.c` ni `build.py`, así que el título y la carpeta de
configuración (`sdmc:/3ds/spotibacan/`) son los mismos: se instala con FBI
encima de la que tenés y conserva el vínculo con tu cuenta.

Si algo falla, el log nuevo del PR agrega por conexión el tiempo de red vs.
CPU — traeme la SD y lo leo.
