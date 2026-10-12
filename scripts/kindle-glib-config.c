/* Build-time measurements for GLib's cross-configure cache, GPL-3.0-or-later.
 * Run only under QEMU with the private target libc, not on a Kindle. */
#include <errno.h>
#include <grp.h>
#include <pthread.h>
#include <pwd.h>
#include <stdio.h>

int main(void) {
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    struct passwd pwd, *pw = NULL;
    struct group grp, *gr = NULL;
    char buffer[10000];
    int result = getpwuid_r(0, &pwd, buffer, sizeof(buffer), &pw);
    printf("ac_cv_func_posix_getpwuid_r=%s\n", result == ENOSYS ? "no" : "yes");
    result = getgrgid_r(0, &grp, buffer, sizeof(buffer), &gr);
    printf("ac_cv_func_posix_getgrgid_r=%s\n", result == ENOSYS ? "no" : "yes");
    printf("glib_cv_sizeof_system_thread=%zu\n", sizeof(pthread_t));
    printf("glib_cv_sizeof_gmutex=%zu\n", sizeof(mutex));
    printf("glib_cv_byte_contents_gmutex='");
    for (size_t i = 0; i < sizeof(mutex); ++i) {
        printf("%s%u", i ? "," : "", ((unsigned char *)&mutex)[i]);
    }
    puts("'");
    return 0;
}
