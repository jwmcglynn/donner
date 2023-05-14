+++
title = "Release Notes"
template = "content-page.html"
weight = 1
date = 2020-01-01
description = "What's changed between each release of hdoc, including new features, fixes, and improvements."
+++

# Version 1.4.1 (6 February 2023)

## Fixes
* Strings in "ignore.paths" are now compared to their relative path instead of the absolute path of the file.
* hdoc's build system will now use an additional method to try to find the system-level Clang libraries it uses as a dependency.
  - Thank you [@no92](https://github.com/no92) for [contributing this fix](https://github.com/hdoc/hdoc/pull/25).

## Internal Changes
* hdoc now uses Meson wraps to vendor its subproject dependencies instead of keeping them in the source tree.

# Version 1.4.0 (24 October 2022)

# New Features
* hdoc now uses JSON serialization/deserialization instead of a binary format when uploading to docs.hdoc.io.
  - JSON can also be dumped to the current working directory by setting the new `debug.dump_json_payload` [configuration option](@/docs/reference/config-file-reference.md#dump-json-payload) to `true`.
* The version of hdoc which is provided as a pre-compiled binary on hdoc.io and uploads documentation to docs.hdoc.io for public hosting is now called `hdoc-online`.
  - This binary was previously called `hdoc` or `hdoc-client` which was confusing.
* There is a new configuration option named `git_default_branch` which allows users to specify the default branch or commit for their project.
  - Thank you [@no92](https://github.com/no92) for [contributing this feature](https://github.com/hdoc/hdoc/pull/32) with a high quality PR including docs!
  - This also fixes a bug where the "Repository" link in the sidebar pointed to an invalid link.
* The Nix flake config is no longer hardcoded for x86_64 Linux.

## Fixes
* Binaries built as part of hdoc are now marked as installable.
  - Thank you [@no92](https://github.com/no92) for [contributing this fix](https://github.com/hdoc/hdoc/pull/33).
* Fixed a bug where the `ignore_private_members` configuration option was being parsed incorrectly.
  - Thank you [@strager](https://github.com/strager) for [reporting this bug](https://github.com/hdoc/hdoc/pull/31).

## Internal Changes
* All of hdoc's unit tests are now bundled in one binary: `hdoc-tests`, as opposed to the previous multitude of test binaries that were built (`index-tests`, `unit-tests`)
* Clang diagnostic output is now ignored.

# Version 1.3.2 (30 September 2022)

## Fixes
* Fixed a bug where the new loading spinner on search pages was not visible in light mode.
  - Thank you [@jtbandes](https://github.com/jtbandes) for [the reporting this bug](https://github.com/hdoc/hdoc/issues/24).

# Version 1.3.1 (29 September 2022)

## New Features
* All third-party JS required for hdoc to work is now bundled in with hdoc instead of being loaded from the internet.
  - Now users can generate and view their documentation without compromises when they're without an internet connection.
  - Thank you [@totph](https://github.com/totph) for the suggestion.
* Users can now limit the number of files that are indexed by hdoc by specifying the `limit_num_indexed_files` variable under the new `[debug]` section of `.hdoc.toml`.
  - This is intended to make hdoc easier to bring in up large projects with thousands of files which can be slow to index. Users can test hdoc on a subset of the project which is faster than waiting for hdoc to index the whole project.
  - More documentation about this feature is available in the [Configuration File Reference](@/docs/reference/config-file-reference.md#limit-num-indexed-files).
  - Thank you [@totph](https://github.com/totph) for the suggestion.
* A spinner was added on the search page while the search index is loading.
  - This provides a visual indication of the loading process, whereas previously there was only a static message indicating that the index was loading.
  - Thank you [@totph](https://github.com/totph) for the suggestion.
* The order of the sidebar was re-arranged, with links to Markdown documentation pages placed at the top, and API reference links at the bottom. This is the reverse of the previous layout and was done to make written documentation easier to access in projects where there are many written docs.
  - Thank you [@totph](https://github.com/totph) for the suggestion.
* The `projectVersion` parameter is now optional.
  - This is useful for projects that have an unversioned main branch from which their documentation is generated.
  - Thank you [@totph](https://github.com/totph) for the suggestion.

## Fixes
* Removed the giant "hdoc" symbol at the top of the documentation sidebar and replaced it with a smaller link at the bottom of the "Navigation" section of the sidebar.
  - Thank you [@jtbandes](https://github.com/jtbandes) for [the suggestion](https://github.com/hdoc/hdoc/issues/19).
* Replaced the short form `-v` of the `--verbose` flag to avoid a conflict with the short form of `--version`
  - Thank you [@totph](https://github.com/totph) for reporting this bug.
* Fixed a crash when `@param` was used in a doc comment but the actual name of the parameter was left empty.
  - Thank you [@totph](https://github.com/totph) for [contributing a fix](https://github.com/hdoc/hdoc/pull/22) (with test cases!) for this bug.
* Fixed an issue where inline command comments, such as `@a`, were being incorrectly parsed and their arguments were being omitted from the HTML output.
  - Thank you [@totph](https://github.com/totph) for reporting this bug.
* Fixed an issue where templated types weren't linked in HTML documentation.
* Fixed an issue where the "Templates" header was the wrong size for Record symbols.

## Internal Changes
* Improvements and optimizations of AST parsing to reduce time spent parsing code.

# Version 1.3.0 (21 July 2022)

## New Features
* hdoc now supports the `@tparam` command, which has the same semantics as it does in Doxygen.
* hdoc now has a dark mode that automatically adapts to the user's system settings.
* Documentation generated by hdoc will now have a small permalink icon at the relevant headers in the documentation to make it easier to share links to specific symbols.

## Fixes
* A bug where links to member functions on the documentation search page would lead to the wrong link has been fixed.
  - Thank you [@jtbandes](https://github.com/jtbandes) for reporting [this issue](https://github.com/hdoc/hdoc/issues/14) and supplying reproduction steps.

## Internal Changes
* Updated to LLVM 14.

# Version 1.2.4 (6 June 2022)

This is a hotfix release.

## New Features
* Only the name of the method is made linkable in the method list under each record.
  - Previously the whole method, including return type and method parameters, was made linkable which was visually busy.

## Fixes
* A bug where hdoc crashed when the `ignore_private_members` configuration variable was set to `true` was fixed.
  - Thank you [@jtbandes](https://github.com/jtbandes) for reporting [this issue](https://github.com/hdoc/hdoc/issues/5#issuecomment-1148031717) and supplying reproduction steps.
* hdoc won't proceed if the output directory can't be created.

# Version 1.2.3 (25 May 2022)

## New Features
* hdoc now has an integration with GitHub to generate documentation directly from GitHub Actions. This makes automatically generating documentation for your project easier than ever.
  - See the [blog post](@/blog/hdoc-github-actions-beta.md) for more details.
* Users can now ignore private member variables and private member functions by setting the `ignore_private_members` variable to `true` in `.hdoc.toml`.
  - By default, private members are included in the documentation which matches the behavior of hdoc prior to this change.
  - More documentation about this feature is available in the [Configuration File Reference](@/docs/reference/config-file-reference.md#ignore-private-members).
  - Thank you to [@jm4games](https://github.com/jm4games) for bringing this issue to our attention and [@jtbandes](https://github.com/jtbandes) for providing the impetus to mainline this.

## Fixes
* Documentation URL is printed to stdout even if `--verbose` option is not specified.
  - This only applies to the prebuilt static binaries built provided by us on hdoc.io

## Internal changes
* The `tiny-process-library` dependency was removed and replaced with LLVM functionality that accomplished the same task.
* Several internal dependencies were updated:
  - toml++ was updated to 3.1.0.
  - doctest was updated to 2.4.8
  - cpp-httplib was updated to 0.10.6
  - cereal was updated to 1.3.2
  - argparse was updated to 2.4

# Version 1.2.2 (17 March 2022)

## New Features
* Unofficial Windows support and build instructions.
  - Thank you [@yqs112358](https://github.com/yqs112358) for contributing this!

## Fixes
* Users will now be warned if `output_dir` is specified in `.hdoc.toml` but they are running the online version of hdoc
  - Thank you [@ShulkMaster](https://github.com/ShulkMaster) for reporting [this bug](https://github.com/hdoc/hdoc/issues/10).
* hdoc will now immediately halt if the `output_dir` option is missing when it is otherwise required.

## Internal changes
* cmark-gfm updated to version 0.29.0.gfm.3 because of [a security vulnerability](https://github.com/github/cmark-gfm/security/advisories/GHSA-mc3g-88wq-6f4x).

# Version 1.2.1 (15 October 2021)

## New Features
* No new features in this release.

## Fixes
* An issue where LaTeX commands such as `\sqrt{}` would not appear properly in the rendered math output has been fixed

## Internal changes
* Updated to LLVM 13.

# Version 1.2.0 (1 July 2021)

## New Features
* hdoc now supports LaTeX math in Markdown pages and documentation comments.
  - KaTeX is used to render math in the generated documentation.
  - Only the `$` (inline math) and `$$` (display math) delimiters are supported.
* hdoc now supports tables in Markdown pages.
* hdoc now prints breadcrumbs at the top of each documentation page for functions, records, and enums.
  - This makes the structure of the code more clear (e.g. what namespaces is this function a part of?)

## Fixes
* [A bug](https://github.com/hdoc/hdoc/issues/4) which misprinted functions with trailing return types and exception specifiers was fixed.
* Markdown documents that have exceptionally wide code blocks are now wrapped to ensure that the entire webpage is not stretched to uncomfortable widths. A horizontal scrollbar now wraps such code blocks.
* Attribution for the open source libraries we use in the web documentation was added to the [open source page](https://hdoc.io/oss/).
  - The newly attributed libraries/tools are: [Bulma](https://bulma.io/), [highlight.js](https://highlightjs.org/), [KaTeX](https://katex.org/), and [minisearch](https://github.com/lucaong/minisearch).
  - Thank you to all the contributors to these projects!
* A message indicating that no free functions are defined in the current project is printed instead of leaving a blank page.
* The project name and version is printed in the page title (i.e. HTML `<title>` tag) of all documentation pages.

## Internal changes
* Integration tests, unit tests, and index tests were all moved to the `tests/` directory.
* The Markdown to HTML library was changed from CommonMark to GitHub's fork of CommonMark to enable the table extension.

# Version 1.1.0 (23 June 2021)

Check out [the accompanying blog post](@/blog/improvements-in-hdoc-1-1-0.md) for more details on the features in hdoc 1.1.0 and videos showing them off.

## New Features
* hdoc now supports HTML links to other symbols defined in a project within the generated documentation.
  - Record member variables and function parameters that are of a known type will automatically have a clickable HTML link taking you directly to their documentation.
  - Many `std` types will also automatically hotlink to their documentation on [cppreference.com](https://en.cppreference.com/w/).
* hdoc now allows you to jump directly to the source code definition of a symbol in GitHub or GitLab from its HTML documentation.
  - hdoc will automatically insert links to the exact file and line where a symbol is stored when you provide a URL to the GitHub or GitLab repository where source code is stored.
  - The URL is specified in the `git_repo_url` option in `.hdoc.toml` See the [configuration file reference](@/docs/reference/config-file-reference.md#git-repo-url) for more information.

## Fixes
* The "Description" heading is now omitted if there are no relevant comments attached to the symbol, whereas previously the "Description" header was printed with nothing below it.
* The message displayed on the Search page to users who do not enable Javascript has been improved and clarified.
* A message is now displayed when no results are found during searches, improving on the empty space that was previously shown.
* The timestamp at the bottom of each generated HTML page is now in UTC instead of EDT.

## Internal Changes
* Many of hdoc's dependencies were updated:
  - LLVM/Clang 12
  - spdlog 1.8.5
  - doctest 2.4.6
  - tiny-process-library v2.0.4
  - toml++ 2.3.0
* A license header indicating hdoc's license and copyright information was added to the top of every source file.
* A single thread pool is now used for all parallel work instead of spinning up and destroying a thread pool for each operation in turn.
* `ParallelExecutor` has been put into the `hdoc::indexer` namespace (previously it was a top-level declaration).
* Improvements to comments and in-source documentation across the project.

# Version 1.0.1 (22 March 2021)

## New Features
* hdoc is now open source and provides free hosting! See the [blog post](@/blog/open-sourcing-hdoc.md) for details.

## Fixes
* hdoc's documentation websites now indicates there were no {Enums,Namespaces,Records} in a project instead of just printing a blank page.
* "Defined at" text was replaced with "Declared at" on HTML pages.

# Version 1.0.0 (4 February 2021)

## New Features
* User documentation is now hosted at docs.hdoc.io.
  - Users must supply the `HDOC_PROJECT_API_KEY` environment variable when running hdoc. The API key is generated when a new project is created in the hdoc console.
  - Documentation is hosted behind a CDN for maximum performance.
  - Enterprise Edition retains the option to output documentation to HTML files on the local filesystem.
* hdoc now uses multiple threads to increase the speed at which it analyzes projects.
  - The `num_threads` option in `.hdoc.toml` controls the number of threads to be used. See the [configuration file reference](@/docs/reference/config-file-reference.md#num-threads) for more information.
  - By default all available threads will be used, corresponding to `num_threads = 0`.
  - Processing speed increase is roughly linear with number of threads, which significantly improves performance on large projects.
* Attribution for open source projects.
  - hdoc relies on several open source projects, and now shows attribution for these projects.
  - Passing the `--oss` flag on the command line will print attribution information.
  - This information is also available on the [hdoc.io website](@/oss.md).
  - To the maintainers and contributors to these projects: thank you!
* [Enterprise Edition] Full support for browsing local documentation using `file:///` URLS.
  - Local HTML output is now fully static and does not require a local HTTP server for most browsing.
  - However, using search functionality still requires a HTTP server.

## Fixes
* Minor whitespace fixes.
  - Several minor fixes to extraneous whitespace that was inserted into the HTML documentation in corner cases.
* It is more accurate to refer to Structs, Classes, and Unions as "Records" instead of using the catch all term "Classes".
  - All references to "Classes" except those that are actually `class` declarations have been renamed to "Records".
* Explicit handling of scoped enums.
  - Scoped enums, or `enum class`, were listed as C-style `enums`. This has been fixed and they are now referred to by the correct name: `enum class`.

# Alpha (17 July 2020)

Initial release.
