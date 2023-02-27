# Donner 🌩 {#mainpage}

[![Build Status](https://github.com/jwmcglynn/donner/actions/workflows/main.yml/badge.svg?branch=main)](https://github.com/jwmcglynn/donner/actions/workflows/main.yml) [![License: ISC](https://img.shields.io/badge/License-ISC-blue.svg)](https://opensource.org/licenses/ISC) [![codecov](https://codecov.io/gh/jwmcglynn/donner/branch/main/graph/badge.svg?token=Z3YJZNKGU0)](https://codecov.io/gh/jwmcglynn/donner)  ![loc](https://img.shields.io/endpoint?url=https://gist.githubusercontent.com/jwmcglynn/91f7f490a72af9c06506c8176729d218/raw/loc.json)
![comments](https://img.shields.io/endpoint?url=https://gist.githubusercontent.com/jwmcglynn/91f7f490a72af9c06506c8176729d218/raw/comments.json)

Donner is a modern C++20 SVG rendering library that:
* Implements **SVG2** natively
* Renders using the world-class **Skia** graphics library
* Allows modifying (and creating) SVG2 files entirely in C++
* Is designed for performance, using game-industry-vetted techniques such as an **EnTT** ECS-backed DOM tree

Donner also provides a production-grade **CSS3** library, which is usable independent of Donner SVG, that provides a hand-written parser, as well as Selectors (Level 4), to not only support parsing a CSS, but also to match it against a tree.

Donner is tested at every layer, and is secure from the start: utilizing fuzzers to ensure that it is resilient to invalid SVG and CSS.
* This is validated with full CI and coverage analysis
## Goals

* Have minimal dependencies, so it can be integrated into existing applications
* Expose the SVG DOM, so that applications can manipulate SVGs dynamically
* Implement the [SVG 2 Specification](https://www.w3.org/TR/SVG/)

## Non-Goals

* Provide a feature-complete implementation of the SVG spec. Donner aims to be compatible, but may implement a subset of the features for performance reasons
* Compile on a broad range of platforms/compilers. Donner is primarily intended as a playground for C++17/C++20, compiling with the latest clang/libc++. (This may be subject to change)

## Other Libraries

* **librsvg**: Provides a simple way to render SVGs one-shot, does not provide a DOM or animation

## History

Donner is a hobby project of mine, which dates back all the way to 2008. At the time, and to some extent today, SVG support as a library in C++ was fairly limited. It was supported by browsers, and big-name vector graphics software, but not as an easily-embeddable rendering library. At the time I was doing game development, and wanted to render vector graphics on-demand instead of shipping rasterized assets.

The original version of Donner SVG from 2008 was never released, but was able to render static SVGs with associated CSS. It used libcairo to render to OpenGL surfaces, and was able to render a good percentage of the SVG 1.1 test suite. It didn't support animation, but extended the SVG format to attach a skeleton for skeletal animation. See more details here [https://jeffmcglynn.com/portfolio/svg-library/](https://jeffmcglynn.com/portfolio/svg-library/).

The original version was incomplete, and was written early in my software development career, and it was not super robust. It was also written before the days of C++11, so it didn't use modern C++ techniques that have changed the landscape of the language. This version of the library was bootstrapped over ten years later, with the intent of rewriting it with industry-grade practices.
