#include <libevdev/libevdev.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>

#define INPUT_DIR "/dev/input"
#define DEBOUNCE_SEC 1

static const char *keyboard_inhibit_path = NULL;

/* -------------------- keyboard path detect -------------------- */
static char* find_keyboard_inhibit() {
    static char path[256];

    DIR *d = opendir(INPUT_DIR);
    if (!d) return NULL;

    struct dirent *de;

    while ((de = readdir(d)) != NULL) {

        if (strncmp(de->d_name, "event", 5) != 0)
            continue;

        char namepath[256];
        snprintf(namepath, sizeof(namepath),
                 "/sys/class/input/%s/device/name",
                 de->d_name);

        FILE *f = fopen(namepath, "r");
        if (!f) continue;

        char name[128] = {0};
        fgets(name, sizeof(name), f);
        fclose(f);

        if (strstr(name, "keyboard") ||
            strstr(name, "AT Translated")) {

            snprintf(path, sizeof(path),
                     "/sys/class/input/%s/device/inhibited",
                     de->d_name);

            closedir(d);
            return path;
        }
    }

    closedir(d);
    return NULL;
}

/* -------------------- tablet device detect -------------------- */
static struct libevdev* find_tablet_device(int *fd_out) {
    DIR *d = opendir(INPUT_DIR);
    if (!d) return NULL;

    struct dirent *de;

    while ((de = readdir(d)) != NULL) {

        if (strncmp(de->d_name, "event", 5) != 0)
            continue;

        char path[256];
        snprintf(path, sizeof(path),
                 "/dev/input/%s", de->d_name);

        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;

        struct libevdev *dev = NULL;

        if (libevdev_new_from_fd(fd, &dev) < 0) {
            close(fd);
            continue;
        }

        if (libevdev_has_event_type(dev, EV_SW) &&
            libevdev_has_event_code(dev, EV_SW, SW_TABLET_MODE)) {

            *fd_out = fd;
            closedir(d);
            return dev;
        }

        libevdev_free(dev);
        close(fd);
    }

    closedir(d);
    return NULL;
}

/* -------------------- inhibit control -------------------- */
static void set_inhibit(int on) {
    if (!keyboard_inhibit_path) return;

    FILE *f = fopen(keyboard_inhibit_path, "w");
    if (!f) return;

    fprintf(f, "%d", on ? 1 : 0);
    fclose(f);
}

/* -------------------- main -------------------- */
int main() {

    int fd;

    keyboard_inhibit_path = find_keyboard_inhibit();
    if (!keyboard_inhibit_path) {
        fprintf(stderr, "keyboard inhibit device not found\n");
        return 1;
    }

    struct libevdev *dev = find_tablet_device(&fd);
    if (!dev) {
        fprintf(stderr, "tablet device not found\n");
        return 1;
    }

    struct input_event ev;

    int last_state = -1;
    time_t last_change = 0;

    while (1) {

        int rc = libevdev_next_event(
            dev,
            LIBEVDEV_READ_FLAG_BLOCKING,
            &ev
        );

        /* -------------------- critical fix: avoid spin -------------------- */
        if (rc < 0) {
            usleep(5000);
            continue;
        }

        if (rc != 0)
            continue;

        if (ev.type != EV_SW ||
            ev.code != SW_TABLET_MODE)
            continue;

        int state = ev.value;
        time_t now = time(NULL);

        /* -------------------- debounce -------------------- */
        if (state == last_state)
            continue;

        if (now - last_change < DEBOUNCE_SEC)
            continue;

        last_state = state;
        last_change = now;

        set_inhibit(state);

    }

    libevdev_free(dev);
    close(fd);
    return 0;
}
