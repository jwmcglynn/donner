# Third-Party Libraries

Third-party libraries are sometimes added as subtrees, and sometimes as bazel external repositories.  If the library is small, it is added as a subtree.

## Subtrees

To list all subtrees, run:
```sh
git log | grep git-subtree-dir | tr -d ' ' | cut -d ":" -f2 | xargs -I {} bash -c 'if [ -d $(git rev-parse --show-toplevel)/{} ] ; then echo {}; fi'
```

The following subtrees have been added to the repo

```sh
git subtree add https://github.com/serge-sans-paille/frozen --prefix third_party/frozen master --squash
git subtree add https://github.com/jwmcglynn/css-parsing-tests --prefix third_party/css-parsing-tests master --squash
git subtree add https://github.com/jwmcglynn/rapidxml_ns --prefix third_party/rapidxml_ns master --squash
```

## Updating

To update a subtree, run the `pull` command and pattern-match from an `add` command above:
```sh
git subtree pull https://github.com/serge-sans-paille/frozen --prefix third_party/frozen master --squash
```
