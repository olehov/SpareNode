# SpareNode agent instructions

## Function discovery

When `tmp/project-function-map.md` exists, consult it before searching the source tree for an
existing function or adding a new function. Use focused searches such as:

```powershell
rg -n -F "function_name" tmp/project-function-map.md
rg -n -F "src/http/" tmp/project-function-map.md
```

Fall back to a repository-wide source search when the map is absent, does not contain the needed
symbol, or records a stale location. A missing local map must never block repository work.

The function map and `tools/function-map/` helper bundle are intentionally local, ignored
developer artifacts and are not distributed by Git. A developer who wants regeneration support
must provision that helper bundle at `tools/function-map/` from their trusted local tooling copy.
When the helpers are available, regenerate the map after a merged pull request or structural code
changes from a PowerShell session inside the checkout:

```powershell
$repositoryRoot = (git rev-parse --show-toplevel).Trim()
Set-Location $repositoryRoot
$wslRepositoryRoot = (wsl.exe wslpath -a $repositoryRoot).Trim()
wsl.exe sh -lc "cd '$wslRepositoryRoot' && doxygen tools/function-map/Doxyfile"
powershell -ExecutionPolicy Bypass -File tools/function-map/generate_project_function_map.ps1
```

