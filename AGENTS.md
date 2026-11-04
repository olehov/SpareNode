# SpareNode agent instructions

## Function discovery

Before searching the source tree for an existing function or adding a new function, consult
`tmp/project-function-map.md` first. Use focused searches such as:

```powershell
rg -n -F "function_name" tmp/project-function-map.md
rg -n -F "src/http/" tmp/project-function-map.md
```

Only fall back to a repository-wide source search when the generated map does not contain the
needed symbol or its recorded location is stale.

The function map is a local, ignored navigation artifact. Regenerate it after a merged pull
request or structural code changes:

```powershell
wsl.exe sh -lc "cd /mnt/e/Projects/SpareNode && doxygen tools/function-map/Doxyfile"
powershell -ExecutionPolicy Bypass -File tools/function-map/generate_project_function_map.ps1
```

