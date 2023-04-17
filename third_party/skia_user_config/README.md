This contains build settings for building the Skia third-party library.

The workspace contains a `local_repository` rule which the Skia repository then uses to build the library:

```
# Clients must specify their own version of skia_user_config to overwrite SkUserConfig.h
local_repository(
    name = "skia_user_config",
    path = "include/config",
)
```
