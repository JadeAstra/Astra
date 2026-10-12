# Grammar corner cases for lambda literals and trailing lambdas: what the spike parser does with each.
param([string]$Astra = "$PSScriptRoot\..\..\build\astra.exe")
$dir = Join-Path ([IO.Path]::GetTempPath()) "astra-m0-probes"
New-Item -ItemType Directory -Force $dir | Out-Null
$prelude = "fun twice(f) { f(); f() }`nfun ready() => true`nval items = [1, 2, 3]`nfun spinner(value, min = 0, max = 100, onChange = {}) => onChange(value + min + max)`n"
$probes = [ordered]@{
    'lambda after a call in an if condition, no parentheses' = "if items.filter { it > 1 }.length == 2 { print('yes') }"
    'the same in parentheses'                                = "if (items.filter { it > 1 }.length == 2) { print('yes') }"
    'for over a call with a lambda, no parentheses'          = "for x in items.map { it * 2 } { write(x, ' ') }"
    'the for loop in parentheses'                           = "for (x in items.map { it * 2 }) { write(x, ' ') }`nprint('')"
    'trailing lambda after named arguments'                 = "print(spinner(3, min = 0, max = 20) { it * 2 })"
    'trailing lambda after positional arguments only'       = "print(spinner(3, 0, 20) { it * 2 })"
    'two lambdas in a row'                                  = "twice { write('a') } { write('b') }"
    'lambda on the next line'                               = "twice`n{ write('block ') }`nprint('')"
    'parameters split over two lines'                       = "val f = { a,`n  b -> a + b }`nprint(f(1, 2))"
    'destructuring parameter'                               = "val f = { (a, b) -> a + b }`nprint(f([1, 2]))"
    'ternary inside a lambda (looks like key: value)'       = "val f = { ready() ? it : 0 }`nprint(f(5))"
    'name then colon is a map, not a lambda'                = "val m = { it: 1 }`nprint(m)"
    'lambda as a map value'                                 = "val m = { 'double': { it * 2 } }`nprint(m['double'](4))"
    'lambdas in a ternary'                                  = "val f = ready() ? { 'yes' } : { 'no' }`nprint(f())"
    'return of a call with a lambda'                        = "fun evens() { return items.filter { it % 2 == 0 } }`nprint(evens())"
    'local with a function type on the first line'          = "val f = { val g : (int) -> int = { it + 1 }; g(1) }`nprint(f())"
    'return inside a lambda (leaves only the lambda today)' = "fun find() {`n  items.each { if it == 2 { return 'found' } }`n  return 'fell through'`n}`nprint(find())"
    'old-style anonymous function still works'              = "print(items.map(fun(x) => x + 1), items.map(fun(x) { return x * 2 }))"
    'is in a condition, then the body'                      = "class A {}`nval a = A()`nif a is A { print('is A') }"
    'a call on a class name in a condition'                 = "class A { static fun ok() => true }`nif A.ok() { print('static ok') }"
    'empty lambda as a default, called with 0 and 1 args'   = "fun button(text, onClick = {}) { onClick(); onClick(1); return text }`nprint(button('OK'))"
    'nested generic type closing with >>'                   = "val m : map<string, list<int>> = { 'a': [1] }`nval n : list<list<int>> = [[1]]`nprint(m, n)"
    'receiver function type as a parameter type'            = "fun build(f : A.() -> Unit) => 1`nclass A {}`nprint(build({}))"
}
"{0,-58} {1}" -f 'case', 'result'
foreach ($name in $probes.Keys) {
    $file = Join-Path $dir 'probe.astra'
    [IO.File]::WriteAllText($file, $prelude + $probes[$name])
    $out = cmd /c "`"$Astra`" `"$file`" 2>&1"
    $ok = $LASTEXITCODE -eq 0
    $text = (($out -join ' | ') -replace 'probe\.astra:', '' -replace '\s+', ' ')
    if ($text.Length -gt 110) { $text = $text.Substring(0, 110) + '...' }
    "{0,-58} {1} {2}" -f $name, $(if ($ok) { 'ok  ' } else { 'FAIL' }), $text
}
