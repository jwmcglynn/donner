"""
cargo-raze crate workspace functions

DO NOT EDIT! Replaced on runs of cargo-raze
"""
load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")
load("@bazel_tools//tools/build_defs/repo:git.bzl", "new_git_repository")

def _new_http_archive(name, **kwargs):
    if not native.existing_rule(name):
        http_archive(name=name, **kwargs)

def _new_git_repository(name, **kwargs):
    if not native.existing_rule(name):
        new_git_repository(name=name, **kwargs)

def raze_fetch_remote_crates():

    _new_http_archive(
        name = "raze__adler32__1_0_4",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/adler32/adler32-1.0.4.crate",
        type = "tar.gz",
        sha256 = "5d2e7343e7fc9de883d1b0341e0b13970f764c14101234857d2ddafa1cb1cac2",
        strip_prefix = "adler32-1.0.4",
        build_file = Label("//third_party/pathfinder/remote:adler32-1.0.4.BUILD"),
    )

    _new_http_archive(
        name = "raze__ahash__0_3_5",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/ahash/ahash-0.3.5.crate",
        type = "tar.gz",
        sha256 = "2f3e0bf23f51883cce372d5d5892211236856e4bb37fb942e1eb135ee0f146e3",
        strip_prefix = "ahash-0.3.5",
        build_file = Label("//third_party/pathfinder/remote:ahash-0.3.5.BUILD"),
    )

    _new_http_archive(
        name = "raze__ansi_term__0_11_0",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/ansi_term/ansi_term-0.11.0.crate",
        type = "tar.gz",
        sha256 = "ee49baf6cb617b853aa8d93bf420db2383fab46d314482ca2803b40d5fde979b",
        strip_prefix = "ansi_term-0.11.0",
        build_file = Label("//third_party/pathfinder/remote:ansi_term-0.11.0.BUILD"),
    )

    _new_http_archive(
        name = "raze__arrayref__0_3_6",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/arrayref/arrayref-0.3.6.crate",
        type = "tar.gz",
        sha256 = "a4c527152e37cf757a3f78aae5a06fbeefdb07ccc535c980a3208ee3060dd544",
        strip_prefix = "arrayref-0.3.6",
        build_file = Label("//third_party/pathfinder/remote:arrayref-0.3.6.BUILD"),
    )

    _new_http_archive(
        name = "raze__arrayvec__0_5_1",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/arrayvec/arrayvec-0.5.1.crate",
        type = "tar.gz",
        sha256 = "cff77d8686867eceff3105329d4698d96c2391c176d5d03adc90c7389162b5b8",
        strip_prefix = "arrayvec-0.5.1",
        build_file = Label("//third_party/pathfinder/remote:arrayvec-0.5.1.BUILD"),
    )

    _new_http_archive(
        name = "raze__atty__0_2_14",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/atty/atty-0.2.14.crate",
        type = "tar.gz",
        sha256 = "d9b39be18770d11421cdb1b9947a45dd3f37e93092cbf377614828a319d5fee8",
        strip_prefix = "atty-0.2.14",
        build_file = Label("//third_party/pathfinder/remote:atty-0.2.14.BUILD"),
    )

    _new_http_archive(
        name = "raze__autocfg__1_0_0",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/autocfg/autocfg-1.0.0.crate",
        type = "tar.gz",
        sha256 = "f8aac770f1885fd7e387acedd76065302551364496e46b3dd00860b2f8359b9d",
        strip_prefix = "autocfg-1.0.0",
        build_file = Label("//third_party/pathfinder/remote:autocfg-1.0.0.BUILD"),
    )

    _new_http_archive(
        name = "raze__base64__0_11_0",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/base64/base64-0.11.0.crate",
        type = "tar.gz",
        sha256 = "b41b7ea54a0c9d92199de89e20e58d49f02f8e699814ef3fdf266f6f748d15c7",
        strip_prefix = "base64-0.11.0",
        build_file = Label("//third_party/pathfinder/remote:base64-0.11.0.BUILD"),
    )

    _new_http_archive(
        name = "raze__bitflags__1_2_1",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/bitflags/bitflags-1.2.1.crate",
        type = "tar.gz",
        sha256 = "cf1de2fe8c75bc145a2f577add951f8134889b4795d47466a54a5c846d691693",
        strip_prefix = "bitflags-1.2.1",
        build_file = Label("//third_party/pathfinder/remote:bitflags-1.2.1.BUILD"),
    )

    _new_http_archive(
        name = "raze__blake2b_simd__0_5_10",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/blake2b_simd/blake2b_simd-0.5.10.crate",
        type = "tar.gz",
        sha256 = "d8fb2d74254a3a0b5cac33ac9f8ed0e44aa50378d9dbb2e5d83bd21ed1dc2c8a",
        strip_prefix = "blake2b_simd-0.5.10",
        build_file = Label("//third_party/pathfinder/remote:blake2b_simd-0.5.10.BUILD"),
    )

    _new_http_archive(
        name = "raze__block__0_1_6",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/block/block-0.1.6.crate",
        type = "tar.gz",
        sha256 = "0d8c1fef690941d3e7788d328517591fecc684c084084702d6ff1641e993699a",
        strip_prefix = "block-0.1.6",
        build_file = Label("//third_party/pathfinder/remote:block-0.1.6.BUILD"),
    )

    _new_http_archive(
        name = "raze__bumpalo__3_4_0",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/bumpalo/bumpalo-3.4.0.crate",
        type = "tar.gz",
        sha256 = "2e8c087f005730276d1096a652e92a8bacee2e2472bcc9715a74d2bec38b5820",
        strip_prefix = "bumpalo-3.4.0",
        build_file = Label("//third_party/pathfinder/remote:bumpalo-3.4.0.BUILD"),
    )

    _new_http_archive(
        name = "raze__bytemuck__1_2_0",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/bytemuck/bytemuck-1.2.0.crate",
        type = "tar.gz",
        sha256 = "37fa13df2292ecb479ec23aa06f4507928bef07839be9ef15281411076629431",
        strip_prefix = "bytemuck-1.2.0",
        build_file = Label("//third_party/pathfinder/remote:bytemuck-1.2.0.BUILD"),
    )

    _new_http_archive(
        name = "raze__byteorder__1_3_4",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/byteorder/byteorder-1.3.4.crate",
        type = "tar.gz",
        sha256 = "08c48aae112d48ed9f069b33538ea9e3e90aa263cfa3d1c24309612b1f7472de",
        strip_prefix = "byteorder-1.3.4",
        build_file = Label("//third_party/pathfinder/remote:byteorder-1.3.4.BUILD"),
    )

    _new_http_archive(
        name = "raze__cbindgen__0_13_2",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/cbindgen/cbindgen-0.13.2.crate",
        type = "tar.gz",
        sha256 = "2db2df1ebc842c41fd2c4ae5b5a577faf63bd5151b953db752fc686812bee318",
        strip_prefix = "cbindgen-0.13.2",
        build_file = Label("//third_party/pathfinder/remote:cbindgen-0.13.2.BUILD"),
    )

    _new_http_archive(
        name = "raze__cc__1_0_54",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/cc/cc-1.0.54.crate",
        type = "tar.gz",
        sha256 = "7bbb73db36c1246e9034e307d0fba23f9a2e251faa47ade70c1bd252220c8311",
        strip_prefix = "cc-1.0.54",
        build_file = Label("//third_party/pathfinder/remote:cc-1.0.54.BUILD"),
    )

    _new_http_archive(
        name = "raze__cfg_if__0_1_10",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/cfg-if/cfg-if-0.1.10.crate",
        type = "tar.gz",
        sha256 = "4785bdd1c96b2a846b2bd7cc02e86b6b3dbf14e7e53446c4f54c92a361040822",
        strip_prefix = "cfg-if-0.1.10",
        build_file = Label("//third_party/pathfinder/remote:cfg-if-0.1.10.BUILD"),
    )

    _new_http_archive(
        name = "raze__cgl__0_3_2",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/cgl/cgl-0.3.2.crate",
        type = "tar.gz",
        sha256 = "0ced0551234e87afee12411d535648dd89d2e7f34c78b753395567aff3d447ff",
        strip_prefix = "cgl-0.3.2",
        build_file = Label("//third_party/pathfinder/remote:cgl-0.3.2.BUILD"),
    )

    _new_http_archive(
        name = "raze__clap__2_33_1",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/clap/clap-2.33.1.crate",
        type = "tar.gz",
        sha256 = "bdfa80d47f954d53a35a64987ca1422f495b8d6483c0fe9f7117b36c2a792129",
        strip_prefix = "clap-2.33.1",
        build_file = Label("//third_party/pathfinder/remote:clap-2.33.1.BUILD"),
    )

    _new_http_archive(
        name = "raze__cmake__0_1_44",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/cmake/cmake-0.1.44.crate",
        type = "tar.gz",
        sha256 = "0e56268c17a6248366d66d4a47a3381369d068cce8409bb1716ed77ea32163bb",
        strip_prefix = "cmake-0.1.44",
        build_file = Label("//third_party/pathfinder/remote:cmake-0.1.44.BUILD"),
    )

    _new_http_archive(
        name = "raze__cocoa__0_19_1",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/cocoa/cocoa-0.19.1.crate",
        type = "tar.gz",
        sha256 = "f29f7768b2d1be17b96158e3285951d366b40211320fb30826a76cb7a0da6400",
        strip_prefix = "cocoa-0.19.1",
        build_file = Label("//third_party/pathfinder/remote:cocoa-0.19.1.BUILD"),
    )

    _new_http_archive(
        name = "raze__constant_time_eq__0_1_5",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/constant_time_eq/constant_time_eq-0.1.5.crate",
        type = "tar.gz",
        sha256 = "245097e9a4535ee1e3e3931fcfcd55a796a44c643e8596ff6566d68f09b87bbc",
        strip_prefix = "constant_time_eq-0.1.5",
        build_file = Label("//third_party/pathfinder/remote:constant_time_eq-0.1.5.BUILD"),
    )

    _new_http_archive(
        name = "raze__core_foundation__0_6_4",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/core-foundation/core-foundation-0.6.4.crate",
        type = "tar.gz",
        sha256 = "25b9e03f145fd4f2bf705e07b900cd41fc636598fe5dc452fd0db1441c3f496d",
        strip_prefix = "core-foundation-0.6.4",
        build_file = Label("//third_party/pathfinder/remote:core-foundation-0.6.4.BUILD"),
    )

    _new_http_archive(
        name = "raze__core_foundation__0_7_0",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/core-foundation/core-foundation-0.7.0.crate",
        type = "tar.gz",
        sha256 = "57d24c7a13c43e870e37c1556b74555437870a04514f7685f5b354e090567171",
        strip_prefix = "core-foundation-0.7.0",
        build_file = Label("//third_party/pathfinder/remote:core-foundation-0.7.0.BUILD"),
    )

    _new_http_archive(
        name = "raze__core_foundation_sys__0_6_2",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/core-foundation-sys/core-foundation-sys-0.6.2.crate",
        type = "tar.gz",
        sha256 = "e7ca8a5221364ef15ce201e8ed2f609fc312682a8f4e0e3d4aa5879764e0fa3b",
        strip_prefix = "core-foundation-sys-0.6.2",
        build_file = Label("//third_party/pathfinder/remote:core-foundation-sys-0.6.2.BUILD"),
    )

    _new_http_archive(
        name = "raze__core_foundation_sys__0_7_0",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/core-foundation-sys/core-foundation-sys-0.7.0.crate",
        type = "tar.gz",
        sha256 = "b3a71ab494c0b5b860bdc8407ae08978052417070c2ced38573a9157ad75b8ac",
        strip_prefix = "core-foundation-sys-0.7.0",
        build_file = Label("//third_party/pathfinder/remote:core-foundation-sys-0.7.0.BUILD"),
    )

    _new_http_archive(
        name = "raze__core_graphics__0_17_3",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/core-graphics/core-graphics-0.17.3.crate",
        type = "tar.gz",
        sha256 = "56790968ab1c8a1202a102e6de05fc6e1ec87da99e4e93e9a7d13efbfc1e95a9",
        strip_prefix = "core-graphics-0.17.3",
        build_file = Label("//third_party/pathfinder/remote:core-graphics-0.17.3.BUILD"),
    )

    _new_http_archive(
        name = "raze__core_graphics__0_19_0",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/core-graphics/core-graphics-0.19.0.crate",
        type = "tar.gz",
        sha256 = "59e78b2e0aaf43f08e7ae0d6bc96895ef72ff0921c7d4ff4762201b2dba376dd",
        strip_prefix = "core-graphics-0.19.0",
        build_file = Label("//third_party/pathfinder/remote:core-graphics-0.19.0.BUILD"),
    )

    _new_http_archive(
        name = "raze__core_text__13_3_2",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/core-text/core-text-13.3.2.crate",
        type = "tar.gz",
        sha256 = "db84654ad95211c082cf9795f6f83dc17d0ae6c985ac1b906369dc7384ed346d",
        strip_prefix = "core-text-13.3.2",
        build_file = Label("//third_party/pathfinder/remote:core-text-13.3.2.BUILD"),
    )

    _new_http_archive(
        name = "raze__core_text__15_0_0",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/core-text/core-text-15.0.0.crate",
        type = "tar.gz",
        sha256 = "131b3fd1f8bd5db9f2b398fa4fdb6008c64afc04d447c306ac2c7e98fba2a61d",
        strip_prefix = "core-text-15.0.0",
        build_file = Label("//third_party/pathfinder/remote:core-text-15.0.0.BUILD"),
    )

    _new_http_archive(
        name = "raze__crc32fast__1_2_0",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/crc32fast/crc32fast-1.2.0.crate",
        type = "tar.gz",
        sha256 = "ba125de2af0df55319f41944744ad91c71113bf74a4646efff39afe1f6842db1",
        strip_prefix = "crc32fast-1.2.0",
        build_file = Label("//third_party/pathfinder/remote:crc32fast-1.2.0.BUILD"),
    )

    _new_http_archive(
        name = "raze__crossbeam_channel__0_4_2",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/crossbeam-channel/crossbeam-channel-0.4.2.crate",
        type = "tar.gz",
        sha256 = "cced8691919c02aac3cb0a1bc2e9b73d89e832bf9a06fc579d4e71b68a2da061",
        strip_prefix = "crossbeam-channel-0.4.2",
        build_file = Label("//third_party/pathfinder/remote:crossbeam-channel-0.4.2.BUILD"),
    )

    _new_http_archive(
        name = "raze__crossbeam_deque__0_7_3",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/crossbeam-deque/crossbeam-deque-0.7.3.crate",
        type = "tar.gz",
        sha256 = "9f02af974daeee82218205558e51ec8768b48cf524bd01d550abe5573a608285",
        strip_prefix = "crossbeam-deque-0.7.3",
        build_file = Label("//third_party/pathfinder/remote:crossbeam-deque-0.7.3.BUILD"),
    )

    _new_http_archive(
        name = "raze__crossbeam_epoch__0_8_2",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/crossbeam-epoch/crossbeam-epoch-0.8.2.crate",
        type = "tar.gz",
        sha256 = "058ed274caafc1f60c4997b5fc07bf7dc7cca454af7c6e81edffe5f33f70dace",
        strip_prefix = "crossbeam-epoch-0.8.2",
        build_file = Label("//third_party/pathfinder/remote:crossbeam-epoch-0.8.2.BUILD"),
    )

    _new_http_archive(
        name = "raze__crossbeam_queue__0_2_2",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/crossbeam-queue/crossbeam-queue-0.2.2.crate",
        type = "tar.gz",
        sha256 = "ab6bffe714b6bb07e42f201352c34f51fefd355ace793f9e638ebd52d23f98d2",
        strip_prefix = "crossbeam-queue-0.2.2",
        build_file = Label("//third_party/pathfinder/remote:crossbeam-queue-0.2.2.BUILD"),
    )

    _new_http_archive(
        name = "raze__crossbeam_utils__0_7_2",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/crossbeam-utils/crossbeam-utils-0.7.2.crate",
        type = "tar.gz",
        sha256 = "c3c7c73a2d1e9fc0886a08b93e98eb643461230d5f1925e4036204d5f2e261a8",
        strip_prefix = "crossbeam-utils-0.7.2",
        build_file = Label("//third_party/pathfinder/remote:crossbeam-utils-0.7.2.BUILD"),
    )

    _new_http_archive(
        name = "raze__deflate__0_8_4",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/deflate/deflate-0.8.4.crate",
        type = "tar.gz",
        sha256 = "e7e5d2a2273fed52a7f947ee55b092c4057025d7a3e04e5ecdbd25d6c3fb1bd7",
        strip_prefix = "deflate-0.8.4",
        build_file = Label("//third_party/pathfinder/remote:deflate-0.8.4.BUILD"),
    )

    _new_http_archive(
        name = "raze__dirs__2_0_2",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/dirs/dirs-2.0.2.crate",
        type = "tar.gz",
        sha256 = "13aea89a5c93364a98e9b37b2fa237effbb694d5cfe01c5b70941f7eb087d5e3",
        strip_prefix = "dirs-2.0.2",
        build_file = Label("//third_party/pathfinder/remote:dirs-2.0.2.BUILD"),
    )

    _new_http_archive(
        name = "raze__dirs_sys__0_3_5",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/dirs-sys/dirs-sys-0.3.5.crate",
        type = "tar.gz",
        sha256 = "8e93d7f5705de3e49895a2b5e0b8855a1c27f080192ae9c32a6432d50741a57a",
        strip_prefix = "dirs-sys-0.3.5",
        build_file = Label("//third_party/pathfinder/remote:dirs-sys-0.3.5.BUILD"),
    )

    _new_http_archive(
        name = "raze__dwrote__0_10_0",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/dwrote/dwrote-0.10.0.crate",
        type = "tar.gz",
        sha256 = "bcdf488e3a52a7aa30a05732a3e58420e22acb4b2b75635a561fc6ffbcab59ef",
        strip_prefix = "dwrote-0.10.0",
        build_file = Label("//third_party/pathfinder/remote:dwrote-0.10.0.BUILD"),
    )

    _new_http_archive(
        name = "raze__either__1_5_3",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/either/either-1.5.3.crate",
        type = "tar.gz",
        sha256 = "bb1f6b1ce1c140482ea30ddd3335fc0024ac7ee112895426e0a629a6c20adfe3",
        strip_prefix = "either-1.5.3",
        build_file = Label("//third_party/pathfinder/remote:either-1.5.3.BUILD"),
    )

    _new_http_archive(
        name = "raze__float_ord__0_2_0",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/float-ord/float-ord-0.2.0.crate",
        type = "tar.gz",
        sha256 = "7bad48618fdb549078c333a7a8528acb57af271d0433bdecd523eb620628364e",
        strip_prefix = "float-ord-0.2.0",
        build_file = Label("//third_party/pathfinder/remote:float-ord-0.2.0.BUILD"),
    )

    _new_git_repository(
        name = "raze__font_kit__0_6_0",
        remote = "https://github.com/jwmcglynn/font-kit.git",
        commit = "b24a6542405294aae5b5f933b0b0d4a982c202e3",
        build_file = Label("//third_party/pathfinder/remote:font-kit-0.6.0.BUILD"),
        init_submodules = True,
    )

    _new_http_archive(
        name = "raze__foreign_types__0_3_2",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/foreign-types/foreign-types-0.3.2.crate",
        type = "tar.gz",
        sha256 = "f6f339eb8adc052cd2ca78910fda869aefa38d22d5cb648e6485e4d3fc06f3b1",
        strip_prefix = "foreign-types-0.3.2",
        build_file = Label("//third_party/pathfinder/remote:foreign-types-0.3.2.BUILD"),
    )

    _new_http_archive(
        name = "raze__foreign_types_shared__0_1_1",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/foreign-types-shared/foreign-types-shared-0.1.1.crate",
        type = "tar.gz",
        sha256 = "00b0228411908ca8685dba7fc2cdd70ec9990a6e753e89b6ac91a84c40fbaf4b",
        strip_prefix = "foreign-types-shared-0.1.1",
        build_file = Label("//third_party/pathfinder/remote:foreign-types-shared-0.1.1.BUILD"),
    )

    _new_http_archive(
        name = "raze__freetype__0_4_1",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/freetype/freetype-0.4.1.crate",
        type = "tar.gz",
        sha256 = "11926b2b410b469d0e9399eca4cbbe237a9ef02176c485803b29216307e8e028",
        strip_prefix = "freetype-0.4.1",
        build_file = Label("//third_party/pathfinder/remote:freetype-0.4.1.BUILD"),
    )

    _new_http_archive(
        name = "raze__fxhash__0_2_1",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/fxhash/fxhash-0.2.1.crate",
        type = "tar.gz",
        sha256 = "c31b6d751ae2c7f11320402d34e41349dd1016f8d5d45e48c4312bc8625af50c",
        strip_prefix = "fxhash-0.2.1",
        build_file = Label("//third_party/pathfinder/remote:fxhash-0.2.1.BUILD"),
    )

    _new_http_archive(
        name = "raze__getrandom__0_1_14",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/getrandom/getrandom-0.1.14.crate",
        type = "tar.gz",
        sha256 = "7abc8dd8451921606d809ba32e95b6111925cd2906060d2dcc29c070220503eb",
        strip_prefix = "getrandom-0.1.14",
        build_file = Label("//third_party/pathfinder/remote:getrandom-0.1.14.BUILD"),
    )

    _new_http_archive(
        name = "raze__gl__0_14_0",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/gl/gl-0.14.0.crate",
        type = "tar.gz",
        sha256 = "a94edab108827d67608095e269cf862e60d920f144a5026d3dbcfd8b877fb404",
        strip_prefix = "gl-0.14.0",
        build_file = Label("//third_party/pathfinder/remote:gl-0.14.0.BUILD"),
    )

    _new_http_archive(
        name = "raze__gl_generator__0_13_1",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/gl_generator/gl_generator-0.13.1.crate",
        type = "tar.gz",
        sha256 = "ca98bbde17256e02d17336a6bdb5a50f7d0ccacee502e191d3e3d0ec2f96f84a",
        strip_prefix = "gl_generator-0.13.1",
        build_file = Label("//third_party/pathfinder/remote:gl_generator-0.13.1.BUILD"),
    )

    _new_http_archive(
        name = "raze__gl_generator__0_14_0",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/gl_generator/gl_generator-0.14.0.crate",
        type = "tar.gz",
        sha256 = "1a95dfc23a2b4a9a2f5ab41d194f8bfda3cabec42af4e39f08c339eb2a0c124d",
        strip_prefix = "gl_generator-0.14.0",
        build_file = Label("//third_party/pathfinder/remote:gl_generator-0.14.0.BUILD"),
    )

    _new_http_archive(
        name = "raze__gleam__0_7_0",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/gleam/gleam-0.7.0.crate",
        type = "tar.gz",
        sha256 = "9ea4f9ba7411ae3f00516401fb811b4f4f37f5c926357f2a033d27f96b74c849",
        strip_prefix = "gleam-0.7.0",
        build_file = Label("//third_party/pathfinder/remote:gleam-0.7.0.BUILD"),
    )

    _new_http_archive(
        name = "raze__half__1_6_0",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/half/half-1.6.0.crate",
        type = "tar.gz",
        sha256 = "d36fab90f82edc3c747f9d438e06cf0a491055896f2a279638bb5beed6c40177",
        strip_prefix = "half-1.6.0",
        build_file = Label("//third_party/pathfinder/remote:half-1.6.0.BUILD"),
    )

    _new_http_archive(
        name = "raze__harfbuzz__0_3_1",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/harfbuzz/harfbuzz-0.3.1.crate",
        type = "tar.gz",
        sha256 = "46f7426266a5ece3e49eae6f48e602c0f8c39917354a847eac9c06437dcde8da",
        strip_prefix = "harfbuzz-0.3.1",
        build_file = Label("//third_party/pathfinder/remote:harfbuzz-0.3.1.BUILD"),
    )

    _new_http_archive(
        name = "raze__harfbuzz_sys__0_3_4",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/harfbuzz-sys/harfbuzz-sys-0.3.4.crate",
        type = "tar.gz",
        sha256 = "212d74cab8498b2d15700b694fb38f77562869d05e1f8b602dd05221a1ca2d63",
        strip_prefix = "harfbuzz-sys-0.3.4",
        build_file = Label("//third_party/pathfinder/remote:harfbuzz-sys-0.3.4.BUILD"),
    )

    _new_http_archive(
        name = "raze__hashbrown__0_7_2",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/hashbrown/hashbrown-0.7.2.crate",
        type = "tar.gz",
        sha256 = "96282e96bfcd3da0d3aa9938bedf1e50df3269b6db08b4876d2da0bb1a0841cf",
        strip_prefix = "hashbrown-0.7.2",
        build_file = Label("//third_party/pathfinder/remote:hashbrown-0.7.2.BUILD"),
    )

    _new_http_archive(
        name = "raze__hermit_abi__0_1_13",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/hermit-abi/hermit-abi-0.1.13.crate",
        type = "tar.gz",
        sha256 = "91780f809e750b0a89f5544be56617ff6b1227ee485bcb06ebe10cdf89bd3b71",
        strip_prefix = "hermit-abi-0.1.13",
        build_file = Label("//third_party/pathfinder/remote:hermit-abi-0.1.13.BUILD"),
    )

    _new_http_archive(
        name = "raze__image__0_23_4",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/image/image-0.23.4.crate",
        type = "tar.gz",
        sha256 = "9117f4167a8f21fa2bb3f17a652a760acd7572645281c98e3b612a26242c96ee",
        strip_prefix = "image-0.23.4",
        build_file = Label("//third_party/pathfinder/remote:image-0.23.4.BUILD"),
    )

    _new_http_archive(
        name = "raze__inflate__0_4_5",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/inflate/inflate-0.4.5.crate",
        type = "tar.gz",
        sha256 = "1cdb29978cc5797bd8dcc8e5bf7de604891df2a8dc576973d71a281e916db2ff",
        strip_prefix = "inflate-0.4.5",
        build_file = Label("//third_party/pathfinder/remote:inflate-0.4.5.BUILD"),
    )

    _new_http_archive(
        name = "raze__instant__0_1_4",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/instant/instant-0.1.4.crate",
        type = "tar.gz",
        sha256 = "7777a24a1ce5de49fcdde84ec46efa487c3af49d5b6e6e0a50367cc5c1096182",
        strip_prefix = "instant-0.1.4",
        build_file = Label("//third_party/pathfinder/remote:instant-0.1.4.BUILD"),
    )

    _new_http_archive(
        name = "raze__io_surface__0_12_1",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/io-surface/io-surface-0.12.1.crate",
        type = "tar.gz",
        sha256 = "2279a6faecd06034f88218f77f7a767693e0957bce0323a96d92747e2760b445",
        strip_prefix = "io-surface-0.12.1",
        build_file = Label("//third_party/pathfinder/remote:io-surface-0.12.1.BUILD"),
    )

    _new_http_archive(
        name = "raze__itoa__0_4_5",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/itoa/itoa-0.4.5.crate",
        type = "tar.gz",
        sha256 = "b8b7a7c0c47db5545ed3fef7468ee7bb5b74691498139e4b3f6a20685dc6dd8e",
        strip_prefix = "itoa-0.4.5",
        build_file = Label("//third_party/pathfinder/remote:itoa-0.4.5.BUILD"),
    )

    _new_http_archive(
        name = "raze__js_sys__0_3_40",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/js-sys/js-sys-0.3.40.crate",
        type = "tar.gz",
        sha256 = "ce10c23ad2ea25ceca0093bd3192229da4c5b3c0f2de499c1ecac0d98d452177",
        strip_prefix = "js-sys-0.3.40",
        build_file = Label("//third_party/pathfinder/remote:js-sys-0.3.40.BUILD"),
    )

    _new_http_archive(
        name = "raze__khronos_api__3_1_0",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/khronos_api/khronos_api-3.1.0.crate",
        type = "tar.gz",
        sha256 = "e2db585e1d738fc771bf08a151420d3ed193d9d895a36df7f6f8a9456b911ddc",
        strip_prefix = "khronos_api-3.1.0",
        build_file = Label("//third_party/pathfinder/remote:khronos_api-3.1.0.BUILD"),
    )

    _new_http_archive(
        name = "raze__lazy_static__1_4_0",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/lazy_static/lazy_static-1.4.0.crate",
        type = "tar.gz",
        sha256 = "e2abad23fbc42b3700f2f279844dc832adb2b2eb069b2df918f455c4e18cc646",
        strip_prefix = "lazy_static-1.4.0",
        build_file = Label("//third_party/pathfinder/remote:lazy_static-1.4.0.BUILD"),
    )

    _new_http_archive(
        name = "raze__leak__0_1_2",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/leak/leak-0.1.2.crate",
        type = "tar.gz",
        sha256 = "bd100e01f1154f2908dfa7d02219aeab25d0b9c7fa955164192e3245255a0c73",
        strip_prefix = "leak-0.1.2",
        build_file = Label("//third_party/pathfinder/remote:leak-0.1.2.BUILD"),
    )

    _new_http_archive(
        name = "raze__leaky_cow__0_1_1",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/leaky-cow/leaky-cow-0.1.1.crate",
        type = "tar.gz",
        sha256 = "40a8225d44241fd324a8af2806ba635fc7c8a7e9a7de4d5cf3ef54e71f5926fc",
        strip_prefix = "leaky-cow-0.1.1",
        build_file = Label("//third_party/pathfinder/remote:leaky-cow-0.1.1.BUILD"),
    )

    _new_http_archive(
        name = "raze__libc__0_2_71",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/libc/libc-0.2.71.crate",
        type = "tar.gz",
        sha256 = "9457b06509d27052635f90d6466700c65095fdf75409b3fbdd903e988b886f49",
        strip_prefix = "libc-0.2.71",
        build_file = Label("//third_party/pathfinder/remote:libc-0.2.71.BUILD"),
    )

    _new_http_archive(
        name = "raze__log__0_4_8",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/log/log-0.4.8.crate",
        type = "tar.gz",
        sha256 = "14b6052be84e6b71ab17edffc2eeabf5c2c3ae1fdb464aae35ac50c67a44e1f7",
        strip_prefix = "log-0.4.8",
        build_file = Label("//third_party/pathfinder/remote:log-0.4.8.BUILD"),
    )

    _new_http_archive(
        name = "raze__malloc_buf__0_0_6",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/malloc_buf/malloc_buf-0.0.6.crate",
        type = "tar.gz",
        sha256 = "62bb907fe88d54d8d9ce32a3cceab4218ed2f6b7d35617cafe9adf84e43919cb",
        strip_prefix = "malloc_buf-0.0.6",
        build_file = Label("//third_party/pathfinder/remote:malloc_buf-0.0.6.BUILD"),
    )

    _new_http_archive(
        name = "raze__maybe_uninit__2_0_0",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/maybe-uninit/maybe-uninit-2.0.0.crate",
        type = "tar.gz",
        sha256 = "60302e4db3a61da70c0cb7991976248362f30319e88850c487b9b95bbf059e00",
        strip_prefix = "maybe-uninit-2.0.0",
        build_file = Label("//third_party/pathfinder/remote:maybe-uninit-2.0.0.BUILD"),
    )

    _new_http_archive(
        name = "raze__memoffset__0_5_4",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/memoffset/memoffset-0.5.4.crate",
        type = "tar.gz",
        sha256 = "b4fc2c02a7e374099d4ee95a193111f72d2110197fe200272371758f6c3643d8",
        strip_prefix = "memoffset-0.5.4",
        build_file = Label("//third_party/pathfinder/remote:memoffset-0.5.4.BUILD"),
    )

    _new_http_archive(
        name = "raze__metal__0_17_1",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/metal/metal-0.17.1.crate",
        type = "tar.gz",
        sha256 = "f83c7dcc2038e12f68493fa3de44235df27b2497178e257185b4b5b5d028a1e4",
        strip_prefix = "metal-0.17.1",
        build_file = Label("//third_party/pathfinder/remote:metal-0.17.1.BUILD"),
    )

    _new_http_archive(
        name = "raze__num_integer__0_1_42",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/num-integer/num-integer-0.1.42.crate",
        type = "tar.gz",
        sha256 = "3f6ea62e9d81a77cd3ee9a2a5b9b609447857f3d358704331e4ef39eb247fcba",
        strip_prefix = "num-integer-0.1.42",
        build_file = Label("//third_party/pathfinder/remote:num-integer-0.1.42.BUILD"),
    )

    _new_http_archive(
        name = "raze__num_iter__0_1_40",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/num-iter/num-iter-0.1.40.crate",
        type = "tar.gz",
        sha256 = "dfb0800a0291891dd9f4fe7bd9c19384f98f7fbe0cd0f39a2c6b88b9868bbc00",
        strip_prefix = "num-iter-0.1.40",
        build_file = Label("//third_party/pathfinder/remote:num-iter-0.1.40.BUILD"),
    )

    _new_http_archive(
        name = "raze__num_rational__0_2_4",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/num-rational/num-rational-0.2.4.crate",
        type = "tar.gz",
        sha256 = "5c000134b5dbf44adc5cb772486d335293351644b801551abe8f75c84cfa4aef",
        strip_prefix = "num-rational-0.2.4",
        build_file = Label("//third_party/pathfinder/remote:num-rational-0.2.4.BUILD"),
    )

    _new_http_archive(
        name = "raze__num_traits__0_2_11",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/num-traits/num-traits-0.2.11.crate",
        type = "tar.gz",
        sha256 = "c62be47e61d1842b9170f0fdeec8eba98e60e90e5446449a0545e5152acd7096",
        strip_prefix = "num-traits-0.2.11",
        build_file = Label("//third_party/pathfinder/remote:num-traits-0.2.11.BUILD"),
    )

    _new_http_archive(
        name = "raze__num_cpus__1_13_0",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/num_cpus/num_cpus-1.13.0.crate",
        type = "tar.gz",
        sha256 = "05499f3756671c15885fee9034446956fff3f243d6077b91e5767df161f766b3",
        strip_prefix = "num_cpus-1.13.0",
        build_file = Label("//third_party/pathfinder/remote:num_cpus-1.13.0.BUILD"),
    )

    _new_http_archive(
        name = "raze__objc__0_2_7",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/objc/objc-0.2.7.crate",
        type = "tar.gz",
        sha256 = "915b1b472bc21c53464d6c8461c9d3af805ba1ef837e1cac254428f4a77177b1",
        strip_prefix = "objc-0.2.7",
        build_file = Label("//third_party/pathfinder/remote:objc-0.2.7.BUILD"),
    )

    _new_http_archive(
        name = "raze__objc_exception__0_1_2",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/objc_exception/objc_exception-0.1.2.crate",
        type = "tar.gz",
        sha256 = "ad970fb455818ad6cba4c122ad012fae53ae8b4795f86378bce65e4f6bab2ca4",
        strip_prefix = "objc_exception-0.1.2",
        build_file = Label("//third_party/pathfinder/remote:objc_exception-0.1.2.BUILD"),
    )

    _new_git_repository(
        name = "raze__pathfinder_c__0_1_0",
        remote = "https://github.com/servo/pathfinder.git",
        commit = "0f3500921596bdb2924d7bd62c4f983afc9332ec",
        build_file = Label("//third_party/pathfinder/remote:pathfinder_c-0.1.0.BUILD"),
        init_submodules = True,
        patches = [
            "//third_party/pathfinder:pathfinder_c.patch",
        ],
        patch_args = [
            "-p1",
        ],
    )

    _new_git_repository(
        name = "raze__pathfinder_canvas__0_5_0",
        remote = "https://github.com/servo/pathfinder.git",
        commit = "0f3500921596bdb2924d7bd62c4f983afc9332ec",
        build_file = Label("//third_party/pathfinder/remote:pathfinder_canvas-0.5.0.BUILD"),
        init_submodules = True,
    )

    _new_git_repository(
        name = "raze__pathfinder_color__0_5_0",
        remote = "https://github.com/servo/pathfinder.git",
        commit = "0f3500921596bdb2924d7bd62c4f983afc9332ec",
        build_file = Label("//third_party/pathfinder/remote:pathfinder_color-0.5.0.BUILD"),
        init_submodules = True,
    )

    _new_git_repository(
        name = "raze__pathfinder_content__0_5_0",
        remote = "https://github.com/servo/pathfinder.git",
        commit = "0f3500921596bdb2924d7bd62c4f983afc9332ec",
        build_file = Label("//third_party/pathfinder/remote:pathfinder_content-0.5.0.BUILD"),
        init_submodules = True,
    )

    _new_git_repository(
        name = "raze__pathfinder_geometry__0_5_1",
        remote = "https://github.com/servo/pathfinder.git",
        commit = "0f3500921596bdb2924d7bd62c4f983afc9332ec",
        build_file = Label("//third_party/pathfinder/remote:pathfinder_geometry-0.5.1.BUILD"),
        init_submodules = True,
    )

    _new_git_repository(
        name = "raze__pathfinder_gl__0_5_0",
        remote = "https://github.com/servo/pathfinder.git",
        commit = "0f3500921596bdb2924d7bd62c4f983afc9332ec",
        build_file = Label("//third_party/pathfinder/remote:pathfinder_gl-0.5.0.BUILD"),
        init_submodules = True,
    )

    _new_git_repository(
        name = "raze__pathfinder_gpu__0_5_0",
        remote = "https://github.com/servo/pathfinder.git",
        commit = "0f3500921596bdb2924d7bd62c4f983afc9332ec",
        build_file = Label("//third_party/pathfinder/remote:pathfinder_gpu-0.5.0.BUILD"),
        init_submodules = True,
    )

    _new_git_repository(
        name = "raze__pathfinder_metal__0_5_1",
        remote = "https://github.com/servo/pathfinder.git",
        commit = "0f3500921596bdb2924d7bd62c4f983afc9332ec",
        build_file = Label("//third_party/pathfinder/remote:pathfinder_metal-0.5.1.BUILD"),
        init_submodules = True,
    )

    _new_git_repository(
        name = "raze__pathfinder_renderer__0_5_0",
        remote = "https://github.com/servo/pathfinder.git",
        commit = "0f3500921596bdb2924d7bd62c4f983afc9332ec",
        build_file = Label("//third_party/pathfinder/remote:pathfinder_renderer-0.5.0.BUILD"),
        init_submodules = True,
    )

    _new_git_repository(
        name = "raze__pathfinder_resources__0_5_0",
        remote = "https://github.com/servo/pathfinder.git",
        commit = "0f3500921596bdb2924d7bd62c4f983afc9332ec",
        build_file = Label("//third_party/pathfinder/remote:pathfinder_resources-0.5.0.BUILD"),
        init_submodules = True,
    )

    _new_git_repository(
        name = "raze__pathfinder_simd__0_5_0",
        remote = "https://github.com/servo/pathfinder.git",
        commit = "0f3500921596bdb2924d7bd62c4f983afc9332ec",
        build_file = Label("//third_party/pathfinder/remote:pathfinder_simd-0.5.0.BUILD"),
        init_submodules = True,
    )

    _new_git_repository(
        name = "raze__pathfinder_text__0_5_0",
        remote = "https://github.com/servo/pathfinder.git",
        commit = "0f3500921596bdb2924d7bd62c4f983afc9332ec",
        build_file = Label("//third_party/pathfinder/remote:pathfinder_text-0.5.0.BUILD"),
        init_submodules = True,
    )

    _new_git_repository(
        name = "raze__pathfinder_ui__0_5_0",
        remote = "https://github.com/servo/pathfinder.git",
        commit = "0f3500921596bdb2924d7bd62c4f983afc9332ec",
        build_file = Label("//third_party/pathfinder/remote:pathfinder_ui-0.5.0.BUILD"),
        init_submodules = True,
    )

    _new_http_archive(
        name = "raze__pkg_config__0_3_17",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/pkg-config/pkg-config-0.3.17.crate",
        type = "tar.gz",
        sha256 = "05da548ad6865900e60eaba7f589cc0783590a92e940c26953ff81ddbab2d677",
        strip_prefix = "pkg-config-0.3.17",
        build_file = Label("//third_party/pathfinder/remote:pkg-config-0.3.17.BUILD"),
    )

    _new_http_archive(
        name = "raze__png__0_16_4",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/png/png-0.16.4.crate",
        type = "tar.gz",
        sha256 = "12faa637ed9ae3d3c881332e54b5ae2dba81cda9fc4bbce0faa1ba53abcead50",
        strip_prefix = "png-0.16.4",
        build_file = Label("//third_party/pathfinder/remote:png-0.16.4.BUILD"),
    )

    _new_http_archive(
        name = "raze__ppv_lite86__0_2_8",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/ppv-lite86/ppv-lite86-0.2.8.crate",
        type = "tar.gz",
        sha256 = "237a5ed80e274dbc66f86bd59c1e25edc039660be53194b5fe0a482e0f2612ea",
        strip_prefix = "ppv-lite86-0.2.8",
        build_file = Label("//third_party/pathfinder/remote:ppv-lite86-0.2.8.BUILD"),
    )

    _new_http_archive(
        name = "raze__proc_macro2__1_0_18",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/proc-macro2/proc-macro2-1.0.18.crate",
        type = "tar.gz",
        sha256 = "beae6331a816b1f65d04c45b078fd8e6c93e8071771f41b8163255bbd8d7c8fa",
        strip_prefix = "proc-macro2-1.0.18",
        build_file = Label("//third_party/pathfinder/remote:proc-macro2-1.0.18.BUILD"),
    )

    _new_http_archive(
        name = "raze__quote__1_0_6",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/quote/quote-1.0.6.crate",
        type = "tar.gz",
        sha256 = "54a21852a652ad6f610c9510194f398ff6f8692e334fd1145fed931f7fbe44ea",
        strip_prefix = "quote-1.0.6",
        build_file = Label("//third_party/pathfinder/remote:quote-1.0.6.BUILD"),
    )

    _new_http_archive(
        name = "raze__rand__0_7_3",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/rand/rand-0.7.3.crate",
        type = "tar.gz",
        sha256 = "6a6b1679d49b24bbfe0c803429aa1874472f50d9b363131f0e89fc356b544d03",
        strip_prefix = "rand-0.7.3",
        build_file = Label("//third_party/pathfinder/remote:rand-0.7.3.BUILD"),
    )

    _new_http_archive(
        name = "raze__rand_chacha__0_2_2",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/rand_chacha/rand_chacha-0.2.2.crate",
        type = "tar.gz",
        sha256 = "f4c8ed856279c9737206bf725bf36935d8666ead7aa69b52be55af369d193402",
        strip_prefix = "rand_chacha-0.2.2",
        build_file = Label("//third_party/pathfinder/remote:rand_chacha-0.2.2.BUILD"),
    )

    _new_http_archive(
        name = "raze__rand_core__0_5_1",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/rand_core/rand_core-0.5.1.crate",
        type = "tar.gz",
        sha256 = "90bde5296fc891b0cef12a6d03ddccc162ce7b2aff54160af9338f8d40df6d19",
        strip_prefix = "rand_core-0.5.1",
        build_file = Label("//third_party/pathfinder/remote:rand_core-0.5.1.BUILD"),
    )

    _new_http_archive(
        name = "raze__rand_hc__0_2_0",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/rand_hc/rand_hc-0.2.0.crate",
        type = "tar.gz",
        sha256 = "ca3129af7b92a17112d59ad498c6f81eaf463253766b90396d39ea7a39d6613c",
        strip_prefix = "rand_hc-0.2.0",
        build_file = Label("//third_party/pathfinder/remote:rand_hc-0.2.0.BUILD"),
    )

    _new_http_archive(
        name = "raze__rayon__1_3_0",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/rayon/rayon-1.3.0.crate",
        type = "tar.gz",
        sha256 = "db6ce3297f9c85e16621bb8cca38a06779ffc31bb8184e1be4bed2be4678a098",
        strip_prefix = "rayon-1.3.0",
        build_file = Label("//third_party/pathfinder/remote:rayon-1.3.0.BUILD"),
    )

    _new_http_archive(
        name = "raze__rayon_core__1_7_0",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/rayon-core/rayon-core-1.7.0.crate",
        type = "tar.gz",
        sha256 = "08a89b46efaf957e52b18062fb2f4660f8b8a4dde1807ca002690868ef2c85a9",
        strip_prefix = "rayon-core-1.7.0",
        build_file = Label("//third_party/pathfinder/remote:rayon-core-1.7.0.BUILD"),
    )

    _new_http_archive(
        name = "raze__redox_syscall__0_1_56",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/redox_syscall/redox_syscall-0.1.56.crate",
        type = "tar.gz",
        sha256 = "2439c63f3f6139d1b57529d16bc3b8bb855230c8efcc5d3a896c8bea7c3b1e84",
        strip_prefix = "redox_syscall-0.1.56",
        build_file = Label("//third_party/pathfinder/remote:redox_syscall-0.1.56.BUILD"),
    )

    _new_http_archive(
        name = "raze__redox_users__0_3_4",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/redox_users/redox_users-0.3.4.crate",
        type = "tar.gz",
        sha256 = "09b23093265f8d200fa7b4c2c76297f47e681c655f6f1285a8780d6a022f7431",
        strip_prefix = "redox_users-0.3.4",
        build_file = Label("//third_party/pathfinder/remote:redox_users-0.3.4.BUILD"),
    )

    _new_http_archive(
        name = "raze__remove_dir_all__0_5_2",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/remove_dir_all/remove_dir_all-0.5.2.crate",
        type = "tar.gz",
        sha256 = "4a83fa3702a688b9359eccba92d153ac33fd2e8462f9e0e3fdf155239ea7792e",
        strip_prefix = "remove_dir_all-0.5.2",
        build_file = Label("//third_party/pathfinder/remote:remove_dir_all-0.5.2.BUILD"),
    )

    _new_http_archive(
        name = "raze__rust_argon2__0_7_0",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/rust-argon2/rust-argon2-0.7.0.crate",
        type = "tar.gz",
        sha256 = "2bc8af4bda8e1ff4932523b94d3dd20ee30a87232323eda55903ffd71d2fb017",
        strip_prefix = "rust-argon2-0.7.0",
        build_file = Label("//third_party/pathfinder/remote:rust-argon2-0.7.0.BUILD"),
    )

    _new_http_archive(
        name = "raze__rustc_version__0_2_3",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/rustc_version/rustc_version-0.2.3.crate",
        type = "tar.gz",
        sha256 = "138e3e0acb6c9fb258b19b67cb8abd63c00679d2851805ea151465464fe9030a",
        strip_prefix = "rustc_version-0.2.3",
        build_file = Label("//third_party/pathfinder/remote:rustc_version-0.2.3.BUILD"),
    )

    _new_http_archive(
        name = "raze__ryu__1_0_5",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/ryu/ryu-1.0.5.crate",
        type = "tar.gz",
        sha256 = "71d301d4193d031abdd79ff7e3dd721168a9572ef3fe51a1517aba235bd8f86e",
        strip_prefix = "ryu-1.0.5",
        build_file = Label("//third_party/pathfinder/remote:ryu-1.0.5.BUILD"),
    )

    _new_http_archive(
        name = "raze__same_file__1_0_6",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/same-file/same-file-1.0.6.crate",
        type = "tar.gz",
        sha256 = "93fc1dc3aaa9bfed95e02e6eadabb4baf7e3078b0bd1b4d7b6b0b68378900502",
        strip_prefix = "same-file-1.0.6",
        build_file = Label("//third_party/pathfinder/remote:same-file-1.0.6.BUILD"),
    )

    _new_http_archive(
        name = "raze__scopeguard__1_1_0",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/scopeguard/scopeguard-1.1.0.crate",
        type = "tar.gz",
        sha256 = "d29ab0c6d3fc0ee92fe66e2d99f700eab17a8d57d1c1d3b748380fb20baa78cd",
        strip_prefix = "scopeguard-1.1.0",
        build_file = Label("//third_party/pathfinder/remote:scopeguard-1.1.0.BUILD"),
    )

    _new_http_archive(
        name = "raze__semver__0_9_0",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/semver/semver-0.9.0.crate",
        type = "tar.gz",
        sha256 = "1d7eb9ef2c18661902cc47e535f9bc51b78acd254da71d375c2f6720d9a40403",
        strip_prefix = "semver-0.9.0",
        build_file = Label("//third_party/pathfinder/remote:semver-0.9.0.BUILD"),
    )

    _new_http_archive(
        name = "raze__semver_parser__0_7_0",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/semver-parser/semver-parser-0.7.0.crate",
        type = "tar.gz",
        sha256 = "388a1df253eca08550bef6c72392cfe7c30914bf41df5269b68cbd6ff8f570a3",
        strip_prefix = "semver-parser-0.7.0",
        build_file = Label("//third_party/pathfinder/remote:semver-parser-0.7.0.BUILD"),
    )

    _new_http_archive(
        name = "raze__serde__1_0_111",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/serde/serde-1.0.111.crate",
        type = "tar.gz",
        sha256 = "c9124df5b40cbd380080b2cc6ab894c040a3070d995f5c9dc77e18c34a8ae37d",
        strip_prefix = "serde-1.0.111",
        build_file = Label("//third_party/pathfinder/remote:serde-1.0.111.BUILD"),
    )

    _new_http_archive(
        name = "raze__serde_derive__1_0_111",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/serde_derive/serde_derive-1.0.111.crate",
        type = "tar.gz",
        sha256 = "3f2c3ac8e6ca1e9c80b8be1023940162bf81ae3cffbb1809474152f2ce1eb250",
        strip_prefix = "serde_derive-1.0.111",
        build_file = Label("//third_party/pathfinder/remote:serde_derive-1.0.111.BUILD"),
    )

    _new_http_archive(
        name = "raze__serde_json__1_0_53",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/serde_json/serde_json-1.0.53.crate",
        type = "tar.gz",
        sha256 = "993948e75b189211a9b31a7528f950c6adc21f9720b6438ff80a7fa2f864cea2",
        strip_prefix = "serde_json-1.0.53",
        build_file = Label("//third_party/pathfinder/remote:serde_json-1.0.53.BUILD"),
    )

    _new_http_archive(
        name = "raze__servo_freetype_sys__4_0_5",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/servo-freetype-sys/servo-freetype-sys-4.0.5.crate",
        type = "tar.gz",
        sha256 = "2c4ccb6d0d32d277d3ef7dea86203d8210945eb7a45fba89dd445b3595dd0dfc",
        strip_prefix = "servo-freetype-sys-4.0.5",
        build_file = Label("//third_party/pathfinder/remote:servo-freetype-sys-4.0.5.BUILD"),
    )

    _new_http_archive(
        name = "raze__skribo__0_1_0",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/skribo/skribo-0.1.0.crate",
        type = "tar.gz",
        sha256 = "e6e9e713ecb4b6d3047428d060aa46cf4abd1109a961da245359e8f88a529d16",
        strip_prefix = "skribo-0.1.0",
        build_file = Label("//third_party/pathfinder/remote:skribo-0.1.0.BUILD"),
    )

    _new_http_archive(
        name = "raze__smallvec__1_4_0",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/smallvec/smallvec-1.4.0.crate",
        type = "tar.gz",
        sha256 = "c7cb5678e1615754284ec264d9bb5b4c27d2018577fd90ac0ceb578591ed5ee4",
        strip_prefix = "smallvec-1.4.0",
        build_file = Label("//third_party/pathfinder/remote:smallvec-1.4.0.BUILD"),
    )

    _new_http_archive(
        name = "raze__strsim__0_8_0",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/strsim/strsim-0.8.0.crate",
        type = "tar.gz",
        sha256 = "8ea5119cdb4c55b55d432abb513a0429384878c15dde60cc77b1c99de1a95a6a",
        strip_prefix = "strsim-0.8.0",
        build_file = Label("//third_party/pathfinder/remote:strsim-0.8.0.BUILD"),
    )

    _new_http_archive(
        name = "raze__syn__1_0_30",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/syn/syn-1.0.30.crate",
        type = "tar.gz",
        sha256 = "93a56fabc59dce20fe48b6c832cc249c713e7ed88fa28b0ee0a3bfcaae5fe4e2",
        strip_prefix = "syn-1.0.30",
        build_file = Label("//third_party/pathfinder/remote:syn-1.0.30.BUILD"),
    )

    _new_http_archive(
        name = "raze__tempfile__3_1_0",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/tempfile/tempfile-3.1.0.crate",
        type = "tar.gz",
        sha256 = "7a6e24d9338a0a5be79593e2fa15a648add6138caa803e2d5bc782c371732ca9",
        strip_prefix = "tempfile-3.1.0",
        build_file = Label("//third_party/pathfinder/remote:tempfile-3.1.0.BUILD"),
    )

    _new_http_archive(
        name = "raze__textwrap__0_11_0",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/textwrap/textwrap-0.11.0.crate",
        type = "tar.gz",
        sha256 = "d326610f408c7a4eb6f51c37c330e496b08506c9457c9d34287ecc38809fb060",
        strip_prefix = "textwrap-0.11.0",
        build_file = Label("//third_party/pathfinder/remote:textwrap-0.11.0.BUILD"),
    )

    _new_http_archive(
        name = "raze__toml__0_5_6",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/toml/toml-0.5.6.crate",
        type = "tar.gz",
        sha256 = "ffc92d160b1eef40665be3a05630d003936a3bc7da7421277846c2613e92c71a",
        strip_prefix = "toml-0.5.6",
        build_file = Label("//third_party/pathfinder/remote:toml-0.5.6.BUILD"),
    )

    _new_http_archive(
        name = "raze__unicode_normalization__0_1_12",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/unicode-normalization/unicode-normalization-0.1.12.crate",
        type = "tar.gz",
        sha256 = "5479532badd04e128284890390c1e876ef7a993d0570b3597ae43dfa1d59afa4",
        strip_prefix = "unicode-normalization-0.1.12",
        build_file = Label("//third_party/pathfinder/remote:unicode-normalization-0.1.12.BUILD"),
    )

    _new_http_archive(
        name = "raze__unicode_width__0_1_7",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/unicode-width/unicode-width-0.1.7.crate",
        type = "tar.gz",
        sha256 = "caaa9d531767d1ff2150b9332433f32a24622147e5ebb1f26409d5da67afd479",
        strip_prefix = "unicode-width-0.1.7",
        build_file = Label("//third_party/pathfinder/remote:unicode-width-0.1.7.BUILD"),
    )

    _new_http_archive(
        name = "raze__unicode_xid__0_2_0",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/unicode-xid/unicode-xid-0.2.0.crate",
        type = "tar.gz",
        sha256 = "826e7639553986605ec5979c7dd957c7895e93eabed50ab2ffa7f6128a75097c",
        strip_prefix = "unicode-xid-0.2.0",
        build_file = Label("//third_party/pathfinder/remote:unicode-xid-0.2.0.BUILD"),
    )

    _new_http_archive(
        name = "raze__vec_map__0_8_2",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/vec_map/vec_map-0.8.2.crate",
        type = "tar.gz",
        sha256 = "f1bddf1187be692e79c5ffeab891132dfb0f236ed36a43c7ed39f1165ee20191",
        strip_prefix = "vec_map-0.8.2",
        build_file = Label("//third_party/pathfinder/remote:vec_map-0.8.2.BUILD"),
    )

    _new_http_archive(
        name = "raze__walkdir__2_3_1",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/walkdir/walkdir-2.3.1.crate",
        type = "tar.gz",
        sha256 = "777182bc735b6424e1a57516d35ed72cb8019d85c8c9bf536dccb3445c1a2f7d",
        strip_prefix = "walkdir-2.3.1",
        build_file = Label("//third_party/pathfinder/remote:walkdir-2.3.1.BUILD"),
    )

    _new_http_archive(
        name = "raze__wasi__0_9_0_wasi_snapshot_preview1",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/wasi/wasi-0.9.0+wasi-snapshot-preview1.crate",
        type = "tar.gz",
        sha256 = "cccddf32554fecc6acb585f82a32a72e28b48f8c4c1883ddfeeeaa96f7d8e519",
        strip_prefix = "wasi-0.9.0+wasi-snapshot-preview1",
        build_file = Label("//third_party/pathfinder/remote:wasi-0.9.0+wasi-snapshot-preview1.BUILD"),
    )

    _new_http_archive(
        name = "raze__wasm_bindgen__0_2_63",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/wasm-bindgen/wasm-bindgen-0.2.63.crate",
        type = "tar.gz",
        sha256 = "4c2dc4aa152834bc334f506c1a06b866416a8b6697d5c9f75b9a689c8486def0",
        strip_prefix = "wasm-bindgen-0.2.63",
        build_file = Label("//third_party/pathfinder/remote:wasm-bindgen-0.2.63.BUILD"),
    )

    _new_http_archive(
        name = "raze__wasm_bindgen_backend__0_2_63",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/wasm-bindgen-backend/wasm-bindgen-backend-0.2.63.crate",
        type = "tar.gz",
        sha256 = "ded84f06e0ed21499f6184df0e0cb3494727b0c5da89534e0fcc55c51d812101",
        strip_prefix = "wasm-bindgen-backend-0.2.63",
        build_file = Label("//third_party/pathfinder/remote:wasm-bindgen-backend-0.2.63.BUILD"),
    )

    _new_http_archive(
        name = "raze__wasm_bindgen_macro__0_2_63",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/wasm-bindgen-macro/wasm-bindgen-macro-0.2.63.crate",
        type = "tar.gz",
        sha256 = "838e423688dac18d73e31edce74ddfac468e37b1506ad163ffaf0a46f703ffe3",
        strip_prefix = "wasm-bindgen-macro-0.2.63",
        build_file = Label("//third_party/pathfinder/remote:wasm-bindgen-macro-0.2.63.BUILD"),
    )

    _new_http_archive(
        name = "raze__wasm_bindgen_macro_support__0_2_63",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/wasm-bindgen-macro-support/wasm-bindgen-macro-support-0.2.63.crate",
        type = "tar.gz",
        sha256 = "3156052d8ec77142051a533cdd686cba889537b213f948cd1d20869926e68e92",
        strip_prefix = "wasm-bindgen-macro-support-0.2.63",
        build_file = Label("//third_party/pathfinder/remote:wasm-bindgen-macro-support-0.2.63.BUILD"),
    )

    _new_http_archive(
        name = "raze__wasm_bindgen_shared__0_2_63",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/wasm-bindgen-shared/wasm-bindgen-shared-0.2.63.crate",
        type = "tar.gz",
        sha256 = "c9ba19973a58daf4db6f352eda73dc0e289493cd29fb2632eb172085b6521acd",
        strip_prefix = "wasm-bindgen-shared-0.2.63",
        build_file = Label("//third_party/pathfinder/remote:wasm-bindgen-shared-0.2.63.BUILD"),
    )

    _new_http_archive(
        name = "raze__web_sys__0_3_40",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/web-sys/web-sys-0.3.40.crate",
        type = "tar.gz",
        sha256 = "7b72fe77fd39e4bd3eaa4412fd299a0be6b3dfe9d2597e2f1c20beb968f41d17",
        strip_prefix = "web-sys-0.3.40",
        build_file = Label("//third_party/pathfinder/remote:web-sys-0.3.40.BUILD"),
    )

    _new_http_archive(
        name = "raze__winapi__0_3_8",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/winapi/winapi-0.3.8.crate",
        type = "tar.gz",
        sha256 = "8093091eeb260906a183e6ae1abdba2ef5ef2257a21801128899c3fc699229c6",
        strip_prefix = "winapi-0.3.8",
        build_file = Label("//third_party/pathfinder/remote:winapi-0.3.8.BUILD"),
    )

    _new_http_archive(
        name = "raze__winapi_i686_pc_windows_gnu__0_4_0",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/winapi-i686-pc-windows-gnu/winapi-i686-pc-windows-gnu-0.4.0.crate",
        type = "tar.gz",
        sha256 = "ac3b87c63620426dd9b991e5ce0329eff545bccbbb34f3be09ff6fb6ab51b7b6",
        strip_prefix = "winapi-i686-pc-windows-gnu-0.4.0",
        build_file = Label("//third_party/pathfinder/remote:winapi-i686-pc-windows-gnu-0.4.0.BUILD"),
    )

    _new_http_archive(
        name = "raze__winapi_util__0_1_5",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/winapi-util/winapi-util-0.1.5.crate",
        type = "tar.gz",
        sha256 = "70ec6ce85bb158151cae5e5c87f95a8e97d2c0c4b001223f33a334e3ce5de178",
        strip_prefix = "winapi-util-0.1.5",
        build_file = Label("//third_party/pathfinder/remote:winapi-util-0.1.5.BUILD"),
    )

    _new_http_archive(
        name = "raze__winapi_x86_64_pc_windows_gnu__0_4_0",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/winapi-x86_64-pc-windows-gnu/winapi-x86_64-pc-windows-gnu-0.4.0.crate",
        type = "tar.gz",
        sha256 = "712e227841d057c1ee1cd2fb22fa7e5a5461ae8e48fa2ca79ec42cfc1931183f",
        strip_prefix = "winapi-x86_64-pc-windows-gnu-0.4.0",
        build_file = Label("//third_party/pathfinder/remote:winapi-x86_64-pc-windows-gnu-0.4.0.BUILD"),
    )

    _new_http_archive(
        name = "raze__wio__0_2_2",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/wio/wio-0.2.2.crate",
        type = "tar.gz",
        sha256 = "5d129932f4644ac2396cb456385cbf9e63b5b30c6e8dc4820bdca4eb082037a5",
        strip_prefix = "wio-0.2.2",
        build_file = Label("//third_party/pathfinder/remote:wio-0.2.2.BUILD"),
    )

    _new_http_archive(
        name = "raze__xml_rs__0_8_3",
        url = "https://crates-io.s3-us-west-1.amazonaws.com/crates/xml-rs/xml-rs-0.8.3.crate",
        type = "tar.gz",
        sha256 = "b07db065a5cf61a7e4ba64f29e67db906fb1787316516c4e6e5ff0fea1efcd8a",
        strip_prefix = "xml-rs-0.8.3",
        build_file = Label("//third_party/pathfinder/remote:xml-rs-0.8.3.BUILD"),
    )

