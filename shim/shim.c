// Preloaded into MAX (LD_PRELOAD). MAX passes "-platform xcb:..." to Qt
// itself, so neither QT_QPA_PLATFORM nor a "-platform" argument of ours can
// choose Wayland. This rewrites that argument when the application object is
// constructed; MAX's own files stay untouched.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <string.h>

static void use_wayland(int argc, char** argv) {
    for (int i = 0; i + 1 < argc; ++i)
        if (strcmp(argv[i], "-platform") == 0 && strncmp(argv[i + 1], "xcb", 3) == 0) argv[i + 1] = (char*)"wayland";
}

typedef void (*ctor_fn)(void*, int*, char**, int);

// QGuiApplication::QGuiApplication(int&, char**, int)
void _ZN15QGuiApplicationC1ERiPPci(void* self, int* argc, char** argv, int flags) {
    static ctor_fn real;
    if (!real) real = (ctor_fn)dlsym(RTLD_NEXT, "_ZN15QGuiApplicationC1ERiPPci");
    use_wayland(*argc, argv);
    real(self, argc, argv, flags);
}

// QApplication::QApplication(int&, char**, int)
void _ZN12QApplicationC1ERiPPci(void* self, int* argc, char** argv, int flags) {
    static ctor_fn real;
    if (!real) real = (ctor_fn)dlsym(RTLD_NEXT, "_ZN12QApplicationC1ERiPPci");
    use_wayland(*argc, argv);
    real(self, argc, argv, flags);
}
