# Not applied on kernel 6.12

## 0003-use-current-timer-api.patch
Rewrites del_timer_sync()/try_to_del_timer_sync() to the 6.15+ names
(timer_delete_sync()/timer_delete_sync_try()) and from_timer() to
timer_container_of(). Those renames landed in 6.15/6.18; on OpenWrt 25.12's
6.12 kernel the old names are the correct ones and the QSDK source already
uses them, so applying this patch breaks the build.
