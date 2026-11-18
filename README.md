# switch2-lab

> **For developers and people researching homebrew.** This is not a modding
> guide or a collection of ready-to-install tools. Feel free to take the
> concepts, adapt the original code, or package useful pieces into something
> more complete.

Useful tools, scripts, experiments, and other things made while tinkering with
Switch 2 homebrew. This is a place to put work that is useful to me and might
be useful for someone else to inspect, research, adapt, or learn from. It can
hold anything from a small helper script to a larger tool; it is not limited
to formal research projects.

**Almost everything here is vibecoded:** written largely with AI assistance,
with human direction and testing. Expect rough code, mistakes, undocumented
assumptions, and incomplete understanding. I do not have enough confidence in
it to present it as polished software, and I am not committing to packaging it
properly or supporting it.

## What being here means

The tools here are things I have actually used on my own system and found useful
for their stated purpose. For PC tools, that means using them in my own Switch 2
workflow; it does not imply the tool runs on the console. Any testing notes
describe a particular version, setup, and observation.

That is a limited observation, **not a guarantee that it works for you or is
safe**. It does not establish compatibility with other firmware or setups,
long-term reliability, recovery from failure, or safety from online-service
restrictions. Where applicable, mistakes can affect saves, installed content,
homebrew access, or persistent storage. Something working for me does not mean
I fully understand every consequence of what it does.

Treat this as source to examine and a starting point for your own research.
These are informal tools and working examples, not supported releases. There
may be no installer, ready-to-run download, complete instructions, or updates.
Everything is provided as-is, with no guarantee of function or support.

## Projects

| Project | What it does | Observed use |
| --- | --- | --- |
| [HEN command bridge](projects/hen-command-bridge/) | PC command access to a running kaiHEN homebrew session. | Version 0.2.1 used for service commands, file operations, and native-call checks on one console. |

## Licensing

Original code and documentation in this repository are offered under the
[Unlicense](UNLICENSE). To the extent I hold any copyright interest in that
work, I dedicate it to the public domain. I am not reserving exclusive rights
to it or asking for credit for my original contributions.

You are welcome to copy, modify, reuse, package, sell, or build on that work.
Third-party code and dependencies retain their own licenses and any required
notices. This dedication covers only rights I can grant; it does not claim
ownership of anyone else's work or override their terms.
