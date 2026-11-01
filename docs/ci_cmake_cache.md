# CMake consumer caches

Run `tools/ci/cmake_consumer.sh build --cache --parallel 4` to use the pinned compiler
cache and dependency-source cache. Configure, link and both CTests still execute.
CMake build directories and compiled FetchContent trees are not shared between jobs.

The compiler cache has a shared 2 GiB soft LRU target. Downloaded source archives
have a separate 1 GiB soft LRU target; runner storage limits bound total growth.
Set `DONNER_CMAKE_CACHE_ROOT` to a persistent data directory on a managed runner.
Compatible Linux slots may share that directory. Native macOS defaults to its
user's `Library/Caches/donner-cmake-v1`. GitHub-hosted jobs retain independent
storage and never connect to an internal cache service. An unavailable cache
warns and falls back to normal compilation.

Compiler identity includes executable content, platform, architecture, SDK,
workspace and effective compiler/wrapper settings. Environment values are hashed
in memory rather than retained in the identity file. Source revision is not a
namespace input: ccache tracks source and header content itself. Job-private
configuration disables hard links and file cloning and discards inherited
ccache settings. Statistics are per-build, without zeroing shared counters.

Only explicitly qualified generated dependency declarations use cached archives.
Git dependencies require immutable commits and the exact supported declaration
schema; URL archives require their declared SHA256. Every archive is rehashed
before extraction into the fresh build. Patched dependencies, unfamiliar options
and unqualified repositories keep normal FetchContent behavior. Existing extracted
sources are checked before reuse within a build directory.

A shared compiler/source cache assumes mutually trusted writers. Stored hashes
catch accidental corruption; they do not authenticate a hostile writer who can
replace both archive and metadata. Executables and configuration are staged
privately per job, outside the shared data directory.

The optional `ci:cmake-cache-qualification` PR label enables trusted producer and
consumer jobs on distinct runner capabilities. A new run/attempt namespace must
start cold, then reuse at least 80 percent of cacheable compiler calls and at least
one dependency download. Receipts bind the source/head and run/attempt; both jobs
must run both CTests without failures or skips. Verify actual distinct runner
identities separately when accepting a shared-runner deployment.

One native macOS fresh-directory comparison compiled 318 cacheable calls cold in
169.112 seconds for the whole lane, then reused all 318 calls and eight dependency
archives in 16.127 seconds. Both runs executed and passed both tests. This is a
same-source measurement, not a general PR runtime guarantee.
