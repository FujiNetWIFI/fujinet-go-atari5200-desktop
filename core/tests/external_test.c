/* "Use external FujiNet": no runtime of ours is started and the cartridge
   DIALS the chosen address:port. A TCP listener plays the external FujiNet:
   the cartridge connects, speaks SLIP, and the link light follows; when the
   connection is dropped it dials again; a second start on another port
   dials there. A bad address refuses to start. */
#include "test_util.h"
#if defined(_WIN32)
int main(void)
{
    return 77; /* POSIX sockets; Windows has the address unit test only */
}
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

static int listener(int *port)
{
    struct sockaddr_in a;
    socklen_t len = sizeof a;
    int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    CHECK(fd >= 0 && bind(fd, (struct sockaddr *)&a, sizeof a) == 0 && listen(fd, 4) == 0, "listener");
    getsockname(fd, (struct sockaddr *)&a, &len);
    *port = ntohs(a.sin_port);
    fcntl(fd, F_SETFL, O_NONBLOCK);
    return fd;
}

/* Waits up to ms for a connection; its fd or -1. */
static int wait_accept(int lfd, int ms)
{
    int t;
    for (t = 0; t < ms; t += 20) {
        int c = accept(lfd, NULL, NULL);
        if (c >= 0) {
            /* BSD/macOS: an accepted socket inherits the listener's O_NONBLOCK */
            fcntl(c, F_SETFL, fcntl(c, F_GETFL, 0) & ~O_NONBLOCK);
            return c;
        }
        test_sleep_ms(20);
    }
    return -1;
}

int main(void)
{
    a52session *s = test_session("external");
    a52session_start_opts o;
    char addr[64], err[200];
    int port, port2, lfd, lfd2, c, t;
    unsigned char b[64];
    int ext, ai;

    CHECK(s, "new");
    ext = a52session_pref_find("fujinet_external");
    ai = a52session_pref_find("fujinet_external_addr");
    CHECK(ext >= 0 && ai >= 0 && a52session_pref(ai)->type == A52_PREF_TEXT, "prefs");
    CHECK(!a52session_pref_enabled(s, ai), "address greyed while the switch is off");
    a52session_set_int(s, "fujinet_external", 1);
    CHECK(a52session_pref_enabled(s, ai), "address live with the switch on");
    CHECK(a52session_pref_set_text(s, ai, "127.0.0.1:11600", err, sizeof err) == 0, "valid text");
    CHECK(a52session_pref_set_text(s, ai, "nope", err, sizeof err) != 0 && err[0], "bad text");
    CHECK(!strcmp(a52session_pref_get_text(s, ai), "127.0.0.1:11600"), "bad text not stored");

    /* a bad address is refused, not started */
    a52session_set_str(s, "fujinet_external_addr", "not an address");
    a52session_default_opts(s, &o);
    o.enable_audio = o.enable_gamepad = 0;
    CHECK(a52session_start(s, &o) != 0 && !a52session_is_running(s), "bad address starts");

    lfd = listener(&port);
    snprintf(addr, sizeof addr, "127.0.0.1:%d", port);
    a52session_set_str(s, "fujinet_external_addr", addr);
    a52session_default_opts(s, &o);
    o.enable_audio = o.enable_gamepad = 0;
    o.sync_cart = 0;
    CHECK(o.external_fujinet == 1, "opts external");
    CHECK(a52session_start(s, &o) == 0, "start: %s", a52session_last_error(s));
    CHECK(a52session_fujinet_external(s) && !a52session_fujinet_running(s), "external, no runtime of ours");
    CHECK(!strcmp(a52session_fujinet_endpoint(s), addr), "endpoint %s", a52session_fujinet_endpoint(s));
    c = wait_accept(lfd, 30000);
    CHECK(c >= 0, "the cartridge dials %s", addr);
    for (t = 0; t < 10000 && a52session_fujinet_link(s) != 1; t += 20)
        test_sleep_ms(20);
    CHECK(a52session_fujinet_link(s) == 1, "link up once connected");

    /* CONFIG talks: a SLIP frame (0xC0 ...) arrives */
    {
        struct timeval tv = {20, 0};
        ssize_t n;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        n = recv(c, b, sizeof b, 0);
        CHECK(n > 0 && b[0] == 0xC0, "a SLIP frame from the cartridge (%d bytes, first %02X)", (int)n, n > 0 ? b[0] : 0);
    }

    /* the connection drops: the cartridge dials again */
    close(c);
    c = wait_accept(lfd, 30000);
    CHECK(c >= 0, "the cartridge reconnects");
    close(c);
    a52session_stop(s);
    close(lfd);
    CHECK(!a52session_fujinet_external(s), "not external when stopped");

    /* another port */
    lfd2 = listener(&port2);
    snprintf(addr, sizeof addr, "127.0.0.1:%d", port2);
    a52session_set_str(s, "fujinet_external_addr", addr);
    a52session_default_opts(s, &o);
    o.enable_audio = o.enable_gamepad = 0;
    o.sync_cart = 0;
    CHECK(a52session_start(s, &o) == 0, "restart: %s", a52session_last_error(s));
    c = wait_accept(lfd2, 30000);
    CHECK(c >= 0, "the cartridge dials the new %s", addr);
    close(c);
    a52session_stop(s);
    a52session_free(s);
    close(lfd2);
    puts("external: ok");
    return 0;
}
#endif
