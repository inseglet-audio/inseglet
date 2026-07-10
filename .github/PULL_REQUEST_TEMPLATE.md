<!-- Thanks for contributing to Inseglet! Please fill this out so review is quick. -->

## Summary

<!-- What does this PR do, and why? -->

## Related issue

<!-- e.g. Closes #123. For anything non-trivial, please link an issue/discussion agreed in advance. -->

## Type of change

- [ ] Bug fix
- [ ] New tool / resource / prompt
- [ ] DSP / spatial / metering
- [ ] Docs
- [ ] Build / CI / packaging
- [ ] Other:

## Testing

<!-- How did you verify this? -->

- [ ] `ctest --test-dir build -C Release -R unit` passes
- [ ] Added/updated a unit or protocol test for the change
- [ ] Live-verified in REAPER (describe below), if applicable

## Checklist

- [ ] Surface change? Regenerated `docs/REFERENCE.md` (`cmake --build build --target reference-doc`)
- [ ] Every state mutation is wrapped in a single undo block
- [ ] First-party source carries an `SPDX-License-Identifier: MIT` header
- [ ] No secrets, tokens, or absolute local paths committed
- [ ] Commits signed off (`git commit -s`, DCO)
