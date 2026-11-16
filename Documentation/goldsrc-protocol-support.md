# Support for GoldSrc network protocol
For connecting to GoldSrc-based servers, use this command:
```
connect ip:port gs
```

The GameCube port generates its own auth ticket (`cl_ticket_generator revemu2013`, the default) and does not
depend on any external service or companion software. Use `cl_ticket_generator null` to send an empty ticket
to servers that do not check it.

By the way, we encountered that some GoldSrc-based servers are recognizing Xash3D clients as "fake clients" and banning/kicking them. Maybe this problem will be
solved along with better compatibility with GoldSrc behavior, but may be not - we don't know logic behind this fake client checks.
