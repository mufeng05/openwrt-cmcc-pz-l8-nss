# Not applied on kernel 6.12 / firmware 12.2

## 0037-use-current-timer-api.patch
Renames del_timer_sync()/try_to_del_timer_sync() -> timer_delete_sync()/
timer_delete_sync_try() and from_timer() -> timer_container_of(). Those are the
6.15/6.18 spellings; on 6.12 the original names are correct, so the patch
breaks the build.

## 0045-nss_qdisc-poll-per-node-statistics-on-the-11.4-firmware.patch
Adds a stats poller written against the 11.4 NSS firmware and the 6.18 timer
API (timer_container_of). This project runs firmware 12.2 on kernel 6.12, so it
is neither needed nor buildable here.
