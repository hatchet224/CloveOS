# File Explorer integration source

`managed.cpp`, `managed.h`, and the controller fixture are original CloveOS
integration source. They connect the File Explorer to the qualified launcher
display/input and managed foreground lifecycle.

The implementation they integrate is the DinguxCommander fork at commit
`7314f86cc1b5d1c75607120e9c2760261f69b67b`. The local upstream snapshot has no
clear license grant. Its implementation and context patches are not included in
this GPL source import while that permission is unresolved. These adapters alone
do not build the complete File Explorer. Resolve the upstream license and
corresponding-source requirements before distributing the combined executable.
