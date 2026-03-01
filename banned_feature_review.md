# Banned feature review - command log

Commands used to scan for banned features from the Chromium C++ feature list:

- `rg -n "#include <(chrono|regex|exception|random|ratio|thread|mutex|future|condition_variable|filesystem|any|span|bit|format|source_location|coroutine|syncstream|cfenv|fenv|cctype|cwctype|ctype\.h|wctype\.h)>"`
- `rg -n "\bstd::(function|bind|bind_front|shared_ptr|weak_ptr|filesystem|any|byte|bit_cast|to_address|u8string|from_chars|to_chars)\b|\bchar8_t\b|u8'" donner examples tools build_defs`
- `rg -n "\bstd::span\b" donner examples tools build_defs`
- `rg -n "\blong long\b" donner examples tools build_defs`
- `rg -n "inline namespace" donner examples tools build_defs`
- `rg -n "std::aligned_(storage|union)|aligned_storage<|aligned_union<" donner examples tools build_defs`
- `rg -n "std::regex|<regex>" donner examples tools build_defs`
- `rg -n "#include <(thread|mutex|future|condition_variable|barrier|latch|semaphore|stop_token)>" donner examples tools build_defs`
- `rg -n "std::(any|byte|from_chars|to_chars|pmr::memory_resource|polymorphic_allocator|timespec_get|uncaught_exceptions|owner_less|weak_from_this)" donner examples tools build_defs`
