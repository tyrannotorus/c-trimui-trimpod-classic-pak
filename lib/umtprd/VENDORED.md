# uMTP-Responder (umtprd), vendored

- Upstream: https://github.com/viveris/uMTP-Responder
- Tag `umtprd-1.8.1`, commit `8f87f7b19034055e2665eaaf07f84a41aa5f25e0`
- License: GPL-3.0 (`LICENSE`)
- Copied: `src/`, `inc/`, `Makefile`, `LICENSE`, `README.md`

Local changes (marked `Trimpod:`):

- `src/umtprd.c` `main()`: don't exit when the POSIX message queue can't be
  opened (the Brick kernel lacks `CONFIG_POSIX_MQUEUE`); skip the
  single-instance lock instead, and initialise `retcode`, which that path
  can leave unset.
