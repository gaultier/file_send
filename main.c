#include "lib.c"

#ifdef PLATFORM_DARWIN
#include "darwin.c"
#endif

#ifdef PLATFORM_LINUX
#include "linux.c"
#endif

#ifdef PLATFORM_WIN32
#include "win32.c"
#endif

#include "otel.c"
#include "torrent.c"

#ifdef WITH_TESTS
#include "test.c"
#endif

int main(i32 argc, char *argv[]) {
  assert(argv);

  const Env *const env = env_platform_make();
  const Logger logger = logger_make(LogLevelAll, bytes_from_cstr("[main] "));

  const char *const cmd = argc >= 2 ? argv[1] : "";
  const usize arena_cap = 32 * MiB;
  Arena arena = {0};
  assert(ErrKindNone == arena_valloc(env, arena_cap, &arena).kind);

  Arena scratch = {0};
  assert(ErrKindNone == arena_valloc(env, 1 * MiB, &scratch).kind);

  // `FILE_SEND_IO_BACKEND` names one, for comparing them on a platform that has
  // more than one; unset asks the platform for whichever it prefers, which is
  // what anything but a measurement should be doing.
  IoBackend backend = IoBackendDefault;
  {
    const char *const requested = getenv("FILE_SEND_IO_BACKEND");
    if (requested) {
      if (0 == strcmp(requested, "kqueue")) {
        backend = IoBackendKqueue;
      } else if (0 == strcmp(requested, "epoll")) {
        backend = IoBackendEpoll;
      } else if (0 == strcmp(requested, "io_uring")) {
        backend = IoBackendIoUring;
      } else {
        fprintf(stderr, "unknown IO backend: %s\n", requested);
        return 1;
      }
    }
  }

  IO *io = NULL;
  Error err = io_platform_make(&arena, env, backend, &io);
  if (ErrKindNone != err.kind) {
    fprintf(stderr, "IO backend: %s\n", io_backend_to_cstr(backend));
    log_err(&logger, "failed to create the IO implementation for the platform",
            err);
    return 1;
  }

#ifdef WITH_TESTS
  if (0 == strcmp(cmd, "test")) {
    test(argc > 2 ? argv[2] : NULL);
  } else
#endif
      if (0 == strcmp(cmd, "gen-torrent")) {
    if (3 != argc) {
      fprintf(stderr, "missing argument\n");
      return 1;
    }

    const Bytes file_path = {.data = (u8 *)argv[2], .len = strlen(argv[2])};
    Bytes input = {0};

    err = io_map_file_blocking(io, file_path, FileOpenOptionsReadOnly, &input);
    if (ErrKindNone != err.kind) {
      log_err(&logger, "failed to open file", err);
      return 1;
    }

    u8 announce_url_cstr[] = "http://localhost:12345";
    Bytes announce_url =
        bytes_make(announce_url_cstr, sizeof(announce_url_cstr) - 1);

    Bytes torrent_file_data = {0};
    u8 info_hash[SHA256_DIGEST_LENGTH] = {0};
    err = torrent_gen_torrent_file_data(file_path, input, announce_url,
                                        &torrent_file_data, info_hash, scratch,
                                        &arena);
    if (ErrKindNone != err.kind) {
      log_err(&logger, "failed to generate torrent file data", err);
      return 1;
    }

    Bytes torrent_file_path = {0};
    err = path_with_ext(file_path, bytes_from_cstr((char *)"torrent"),
                        PATH_SEPARATOR_UNIX, &torrent_file_path, &arena);
    if (ErrKindNone != err.kind) {
      log_err(&logger, "failed to compute the torrent path", err);
      return 1;
    }

    err =
        io_write_all_to_file_blocking(io, torrent_file_path, torrent_file_data);
    if (ErrKindNone != err.kind) {
      log_err(&logger, "failed to write torrent file", err);
      return 1;
    }
  } else if (0 == strcmp(cmd, "share")) {
    if (3 != argc) {
      fprintf(stderr, "missing argument\n");
      return 1;
    }
    const Bytes file_path = bytes_from_cstr(argv[2]);

    const Bytes file_ext = path_get_ext(file_path, PATH_SEPARATOR_UNIX);
    if (!bytes_eq_cstr(file_ext, ".torrent")) {
      fprintf(stderr, "provided file is not a .torrent file: %s\n", argv[2]);
      return 1;
    }

    Bytes input = {0};

    err = io_map_file_blocking(io, file_path, FileOpenOptionsReadOnly, &input);
    if (ErrKindNone != err.kind) {
      log_err(&logger, "failed to open file", err);
      return 1;
    }

    BencodeValue metainfo_dict = {0};
    err = bencode_parse(&input, &arena, scratch, &metainfo_dict);
    if (ErrKindNone != err.kind) {
      log_err(&logger, "failed to parse .torrent data", err);
      return 1;
    }
    if (input.len > 0) {
      fprintf(stderr, "trailing data in .torrent data\n");
      return 1;
    }

    if (BencodeKindDict != metainfo_dict.kind) {
      fprintf(stderr, "metainfo from .torrent data is not a dictionary\n");
      return 1;
    }
    // TODO: More validation on `metainfo_dict`.
    Bytes info_hash_hex_trunc_bytes = {0};
    u8 info_hash_hex_trunc[40] = {0};

    const BencodeValue *const info_dict =
        torrent_find_info_dict_in_metainfo(metainfo_dict);
    if (!info_dict) {
      fprintf(stderr, "info dict from .torrent data not found\n");
      return 1;
    }

    err = torrent_validate_info_dict(*info_dict);
    if (ErrKindNone != err.kind) {
      log_err(&logger, "invalid info dictionary from .torrent data", err);
      return 1;
    }

    Bytes info_encoded = {0};
    err = bencode_encode(*info_dict, &info_encoded, &scratch);
    if (ErrKindNone != err.kind) {
      log_err(&logger, "failed to encode info", err);
      return 1;
    }

    u8 info_hash[SHA256_DIGEST_LENGTH] = {0};
    sha256_digest(info_encoded, info_hash);

    sha256_encode_hex_trunc(info_hash, info_hash_hex_trunc);

    info_hash_hex_trunc_bytes = (Bytes){.data = info_hash_hex_trunc,
                                        .len = sizeof(info_hash_hex_trunc)};
    fwrite(info_hash_hex_trunc_bytes.data, 1, info_hash_hex_trunc_bytes.len,
           stdout);
    puts("");

    i32 udp_socket = 0;
    {
      Error err_udp = env->udp_multicast_open_ipv4(env, 0, &udp_socket);
      if (ErrKindNone != err_udp.kind) {
        log_err(&logger, "failed to open UDP multicast socket", err_udp);
        return 1;
      }
    }

    const usize peer_port = 12345;

    Bytes udp_msg = {0};
    err = torrent_make_udp_broadcast_message(
        bytes_from_cstr("239.192.152.143:6771"), peer_port,
        info_hash_hex_trunc_bytes, &arena, &udp_msg);
    if (ErrKindNone != err.kind) {
      log_err(&logger, "failed to craft UDP multicast message", err);
      return 1;
    }

    fwrite(udp_msg.data, 1, udp_msg.len, stdout);
    puts("");

    const Ipv4Addr lsd_addr = {
        .ip = 0xefc0988fUL, // 239.192.152.143
        .port = 6771,       // Broadcast port.
    };

    // One datagram, and nothing to get on with until it is out.
    {
      IoOnce once = {0};
      io_once_init(&once);

      Error err_sendto = io->send_to(io, &once.completion, udp_socket, lsd_addr,
                                     udp_msg, io_once_on_done);
      if (ErrKindNone == err_sendto.kind) {
        err_sendto = io_once_wait(io, &once);
      }
      if (ErrKindNone != err_sendto.kind) {
        log_err(&logger, "failed to send UDP multicast message", err_sendto);
        return 1;
      }
      assert(once.res <= udp_msg.len);
    }

    const Ipv4Addr listen_addr = {.port = peer_port, .ip = 0};
    // The raw truncated digest, not `info_hash_hex_trunc_bytes`: LSD announces
    // the info hash as 40 hex characters, but a peer handshake carries the 20
    // bytes those characters spell. `info_hash` outlives the event loop below,
    // so the `Bytes` onto it stays good for as long as any connection does.
    TorrentNetworkCtx ctx = {
        .info_hash = bytes_make(info_hash, TORRENT_INFO_HASH_LEN),
        .log_level_mask = LogLevelError | LogLevelInfo | LogLevelDebug,
    };
    IoServer server = {0};
    Error err_listen = io_listen_and_serve_tcp_ipv4(
        io, &server, &ctx, listen_addr, &logger, torrent_peer_on_accept);
    if (ErrKindNone != err_listen.kind) {
      log_err(&logger, "failed to listen and serve", err_listen);
      return 1;
    }

    // The event loop, and the whole of the program from here: every connection
    // accepted, every byte read and every hang-up is a callback reached from
    // these few lines. It returns when the listener cannot go on.
    //
    // Not `io_run_until`, which waits a fixed tick: a turn here is as long as
    // the nearest peer deadline, so a keep-alive goes out when it is due and a
    // loop with nothing to do wakes a handful of times a minute rather than ten
    // times a second. `torrent_peers_deadlines_run` is where the deciding
    // happens -- the loop only says when.
    Error err_run = {.kind = ErrKindNone};
    while (!server.done) {
      const u64 now_ns = io->monotonic_ns(io);
      const u64 earliest = torrent_peers_deadlines_run(&ctx, io, now_ns);

      err_run = io->run_for_ns(io, torrent_peers_wait_ns(earliest, now_ns));
      if (ErrKindNone != err_run.kind) {
        break;
      }
    }
    if (ErrKindNone != err_run.kind) {
      log_err(&logger, "the event loop stopped", err_run);
      return 1;
    }
    if (ErrKindNone != server.err.kind) {
      log_err(&logger, "the listener stopped", server.err);
      return 1;
    }

  } else if (0 == strcmp(cmd, "otel")) {
    if (3 != argc) {
      fprintf(stderr, "missing argument\n");
      return 1;
    }
    const char *const port_s = argv[2];
    char *port_end = NULL;
    const usize port = strtoull(port_s, &port_end, 10);
    const Ipv4Addr listen_addr = {.port = (u16)port};
    Arena http_arena = {0};
    assert(ErrKindNone == arena_valloc(env, 24 * MiB, &http_arena).kind);

    HttpServer ctx = {0};
    {
      const Error err_init = http_server_init(
          &ctx, http_arena, 1024, LogLevelError | LogLevelInfo | LogLevelDebug,
          env_platform_make());
      if (ErrKindNone != err_init.kind) {
        log_err(&logger, "failed to create the http server", err_init);
        return 1;
      }
    }
    IoServer server = {0};
    Error err_listen = io_listen_and_serve_tcp_ipv4(
        io, &server, &ctx, listen_addr, &logger, otel_on_accept);
    if (ErrKindNone != err_listen.kind) {
      log_err(&logger, "failed to listen and serve", err_listen);
      return 1;
    }

    Error err_run = {.kind = ErrKindNone};
    while (!server.done) {
      // const u64 now_ns = io->monotonic_ns(io);
      // const u64 earliest = http_server_deadlines_run(&ctx, io, now_ns);

      err_run = io->run_for_ns(io, 1000 * 1000 * 1000 /* FIXME */);
      if (ErrKindNone != err_run.kind) {
        break;
      }
    }
    if (ErrKindNone != err_run.kind) {
      log_err(&logger, "the event loop stopped", err_run);
      return 1;
    }
    if (ErrKindNone != server.err.kind) {
      log_err(&logger, "the listener stopped", server.err);
      return 1;
    }
  } else {
    fprintf(stderr, "unknown command: %s\n", cmd);
    exit(1);
  }
}
