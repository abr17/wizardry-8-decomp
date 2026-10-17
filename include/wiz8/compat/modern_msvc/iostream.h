#pragma once

/* msvc2022 port: the pre-standard <iostream.h> was removed from MSVC.
   The only includer (src/sgp/Container.cpp) uses nothing from it, so this
   shim is intentionally empty. Only on the include path of the modern build. */
