"""Regression cases for the mixed-platform coordinator's link options."""

load("@bazel_skylib//lib:unittest.bzl", "asserts", "unittest")
load("//build_defs:rules.bzl", "darwin_ci_link_options")

def _ci_link_options_test_impl(ctx):
    env = unittest.begin(ctx)
    dirs = "/fixture/first:/fixture/second"
    first = ["-L/fixture/first", "-Wl,-rpath,/fixture/first"]
    second = ["-L/fixture/second", "-Wl,-rpath,/fixture/second"]
    user = ["-L/fixture/darwin", "-Wl,-rpath,/fixture/darwin", "-lm"]
    assert_kept = first + user + second
    asserts.equals(env, assert_kept, darwin_ci_link_options(assert_kept, dirs))
    complete = first + second
    asserts.equals(env, complete + user, darwin_ci_link_options(complete + user, ""))
    asserts.equals(env, user + first, darwin_ci_link_options(complete + user + first, dirs))
    asserts.equals(env, user + complete, darwin_ci_link_options(complete + user + complete, dirs))
    return unittest.end(env)

ci_link_options_test = unittest.make(_ci_link_options_test_impl)
