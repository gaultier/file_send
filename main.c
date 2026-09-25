#include "lib.c"

#ifdef PLATFORM_UNIX
#include "unix.c"
#endif

#ifdef PLATFORM_WIN32
#include "win32.c"
#endif

#include "torrent.c"

#ifdef WITH_TESTS
#include "test.c"
#endif

int main(i32 argc, char *argv[]) {
  assert(argv);

  const IO io = io_platform_make();

  const char *const cmd = argc >= 2 ? argv[1] : "";
  const usize arena_cap = 32 * MiB;
  Arena arena = {0};
  assert(ErrKindNone == arena_valloc(&io, arena_cap, &arena).kind);

  Arena scratch = {0};
  assert(ErrKindNone == arena_valloc(&io, 1 * MiB, &scratch).kind);

#ifdef WITH_TESTS
  if (0 == strcmp(cmd, "test")) {
    test(argc > 2 ? argv[2] : NULL);
  } else
#endif
      if (0 == strcmp(cmd, "broadcast")) {
    i32 udp_socket = 0;
    {
      Error err_udp = io.udp_multicast_open_ipv4(&io, 0, &udp_socket);
      if (ErrKindNone != err_udp.kind) {
        error_print("failed to open UDP multicast socket", err_udp);
        return 1;
      }
    }
    {
      const u8 msg[] = "Hello!";
      usize sent = 0;
      const Ipv4Addr lsd_addr = {
          .ip = 0xefc0988fUL, // 239.192.152.143
          .port = 6771,
      };

      Error err_sendto = io.udp_send_to_ipv4(&io, udp_socket, lsd_addr, msg,
                                             sizeof(msg), &sent);
      if (ErrKindNone != err_sendto.kind) {
        error_print("failed to send UDP multicast message", err_sendto);
        return 1;
      }
    }
  } else if (0 == strcmp(cmd, "gen-torrent")) {
    if (3 != argc) {
      fprintf(stderr, "missing argument\n");
      return 1;
    }

    const Slice_u8 file_path = {.data = (u8 *)argv[2], .len = strlen(argv[2])};
    Slice_u8 input = {0};

    Error err = io.map_file(&io, file_path, FileOpenOptionsReadOnly, &input);
    if (ErrKindNone != err.kind) {
      error_print("failed to open file", err);
      return 1;
    }

    u8 announce_url_cstr[] = "http://localhost:12345";
    Slice_u8 announce_url =
        slice_u8_make(announce_url_cstr, sizeof(announce_url_cstr) - 1);

    Slice_u8 torrent_file_data = {0};
    u8 info_hash[SHA256_DIGEST_LENGTH] = {0};
    err = torrent_gen_torrent_file_data(file_path, input, announce_url,
                                        &torrent_file_data, info_hash, scratch,
                                        &arena);
    if (ErrKindNone != err.kind) {
      error_print("failed to generate torrent file data", err);
      return 1;
    }

    Slice_u8 torrent_file_path = {0};
    err = path_with_ext(file_path, slice_u8_from_cstr((char *)"torrent"),
                        PATH_SEPARATOR_UNIX, &torrent_file_path, &arena);
    if (ErrKindNone != err.kind) {
      error_print("failed to compute the torrent path", err);
      return 1;
    }

    err = io.write_all_to_file(&io, torrent_file_path, torrent_file_data);
    if (ErrKindNone != err.kind) {
      error_print("failed to write torrent file", err);
      return 1;
    }
  } else if (0 == strcmp(cmd, "share")) {
    if (3 != argc) {
      fprintf(stderr, "missing argument\n");
      return 1;
    }
    const Slice_u8 file_path = slice_u8_from_cstr(argv[2]);

    const Slice_u8 file_ext = path_get_ext(file_path, PATH_SEPARATOR_UNIX);
    if (!slice_u8_eq_cstr(file_ext, ".torrent")) {
      fprintf(stderr, "provided file is not a .torrent file: %s\n", argv[2]);
      return 1;
    }

    Slice_u8 input = {0};

    Error err = io.map_file(&io, file_path, FileOpenOptionsReadOnly, &input);
    if (ErrKindNone != err.kind) {
      error_print("failed to open file", err);
      return 1;
    }

    const Ipv4Addr listen_addr = {.port = 12345, .ip = 0};
    TorrentNetworkCtx ctx = {0};
    Error err_listen = io_listen_and_serve_tcp_ipv4(&io, &ctx, listen_addr,
                                                    torrent_client_on_accept);
    if (ErrKindNone != err_listen.kind) {
      error_print("failed to listen and serve", err_listen);
      return 1;
    }

    const usize unused_bytes = (usize)arena.end - (usize)arena.start;
    const usize used_bytes = arena_cap - unused_bytes;
    printf("mem used: %zu\n", used_bytes);
    printf("mem unused: %zu\n", unused_bytes);
  } else {
    fprintf(stderr, "unknown command: %s\n", cmd);
    exit(1);
  }
}
