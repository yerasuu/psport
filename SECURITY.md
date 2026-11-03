# Security policy

## Supported versions

psport is below 1.0: only the [latest release](https://github.com/yerasuu/psport/releases/latest)
receives security fixes. Please check that a problem is still present there before reporting it.

## Reporting a vulnerability

Report it privately through GitHub:
[Report a vulnerability](https://github.com/yerasuu/psport/security/advisories/new).
Please do not open a public issue or a pull request for it.

Include what you can of: the release tag or commit, the system and GPU driver, what you
did, what happened, and what you expected. A proof of concept helps. Never attach game files, dumps of the
console or its keys, or account credentials: describe them or redact them.

You will get an answer as soon as it can be given. This is a single-maintainer project, so there is no
fixed response time, but a report is read, confirmed or declined with a reason, and a fix is made public
together with its advisory. You are credited there unless you prefer not to be.

## Scope

In scope: how psport treats input it does not control, on the host side.

- files read from a game's dump (the executable and its modules, `param.sfo`, manifests, saves, patches
  and mods) and from the launcher (`run.sh` variables, `psport.ini`, the `--list-*` and `--check-game`
  queries);
- the network code: the shadNet module, HTTP and TLS, and how the login and its secrets are carried;
- memory-safety and resource problems in the native system libraries that a guest can reach with ordinary
  calls, when they cross from the guest into the host process;
- what logs, call reports and error reports may leak.

Out of scope:

- psport does not sandbox the game: the game's own x86-64 code runs directly on the processor with the
  user's privileges, so a malicious executable doing what the user could do is not a vulnerability. Run
  only dumps you trust;
- the console's firmware or system modules, and games' own bugs;
- an attacker who already has the user's own privileges on the machine;
- an advisory in vendored third-party code that psport neither builds nor ships (the repository's
  [osv-scanner.toml](osv-scanner.toml) names the one such case, the documentation requirements of `half`).
