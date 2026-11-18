# A fix for `connect` moving each sender twice

`git am` patches for NVIDIA/stdexec from 2026-11-07: `default_domain::transform_sender`
returned a sender it didn't transform by value, so `connect` moved every layer's data
twice. `0001` is the fix with tests; `0002` and `0003` are `amend!` commits that only
reword its message (apply them with `git am --allow-empty`, then autosquash).

The patches apply to stdexec 34773fb, the first commit whose pre-image blobs match the
patch's `index` lines (found while importing; the original base wasn't recorded).
