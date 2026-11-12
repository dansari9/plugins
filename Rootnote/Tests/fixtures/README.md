# State fixtures

Real states written by the released code, loaded by the plugin test
"states saved by 0.4.0, 0.4.1 and 0.4.2 load with their values".

| File | Written by |
| --- | --- |
| `state-0.4.0.bin`, `state-0.4.1.bin`, `state-0.4.2.bin` | `writeOldState()` in `generator.inc`: preset 3, then every parameter edited, editor 1200 x 675 |
| `default-0.4.0.bin`, `default-0.4.1.bin`, `default-0.4.2.bin` | A fresh processor's `getStateInformation()` (0.4.0/0.4.1 default MIDI to Ch 1+2, 0.4.2 to Off) |

The releases are commits 9b86c30 (0.4.0), 20d0912 (0.4.1) and 853dd16 (0.4.2).
To regenerate: `git worktree add` the commit, paste `generator.inc` above
`main()` in its `Tests/PluginTests.cpp`, call it from `main()`, build
`RootnotePluginTests` and run it. The three edited states are byte-identical:
those releases wrote the same format.
