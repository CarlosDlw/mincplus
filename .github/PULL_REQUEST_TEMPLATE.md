<!--
One topic per pull request. The description is what a reader of `git log` sees
in ten years, so it says what changed and why -- the alternative that was
rejected, the bug the refactor closed, the measurement that decided it.
-->

## What this changes

<!-- A paragraph. If it closes an issue, say `Closes #123`. -->

## Why

<!--
The reason, not the summary. If a rule moved, say which stage owns it now and
what was wrong with two stages owning it. If a diagnostic changed, show the
before and after.
-->

## Checklist

- [ ] One topic, and the commit messages say what and why
- [ ] `make gates` is green (ci, sanitize, format, tidy)
- [ ] `make examples` is green — required if anything in the front end, the
      lowering or the backend changed
- [ ] A test per stage the change touches, **including the case that must not
      work**: the type that must not convert, the body that must not compile, the
      offset that must not be accepted
- [ ] An example in `examples/` if the change is visible to a `.mx` file
- [ ] The documentation is updated — the site in `website/` for the language,
      `docs/` for the design, and `README.md`'s checklist if a feature's status
      changed
- [ ] Nothing unimplemented is described as if it worked, anywhere

## Notes for the reviewer

<!--
Anything that will save the reviewer time: the part you are least sure of, the
file that got bigger, the test that is doing more work than it looks like.
-->
