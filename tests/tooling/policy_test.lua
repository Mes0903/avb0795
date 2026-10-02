package.path = './lib/?.lua;./vendor/?.lua;../tests/tooling/?.lua'
package.cpath = ''

local fixtures = require('inventory_fixtures')
local policy = require('test_inventory')
local profiles = require('inventory_profiles')
local json = require('dkjson')

local groups = 0

local function check(name, mutate, expected, needle)
  local document = fixtures.inventory(name)
  mutate(document)
  local document_before = assert(json.encode(document))
  local profile_before = assert(json.encode(profiles[name]))
  local errors = policy.validate_inventory(document, profiles[name])
  assert((#errors == 0) == expected,
    string.format('%s: expected accepted=%s, got %s', name, tostring(expected), table.concat(errors, ' | ')))
  assert(json.encode(document) == document_before, name .. ': document mutated')
  assert(json.encode(profiles[name]) == profile_before, name .. ': profile mutated')
  if needle then
    assert(string.find(table.concat(errors, '\n'), needle, 1, true),
      string.format('%s: missing diagnostic %q in %s', name, needle, table.concat(errors, ' | ')))
  end
end

local function group(run)
  run()
  groups = groups + 1
end

group(function()
  check('windows-v1-full', function(document)
    document.tests[1], document.tests[15] = document.tests[15], document.tests[1]
    document.extra = 'allowed'
    document.tests[1].properties[1].value[#document.tests[1].properties[1].value + 1] = 'extra-label'
  end, true)
end)

group(function()
  for _, name in ipairs({ 'windows-v1-full', 'windows-policy', 'portable-core' }) do
    check(name, function() end, true)
  end
  check('windows-v1-full', function(document)
    document.tests = fixtures.inventory('windows-policy').tests
  end, false)
  check('windows-policy', function(document)
    document.tests = fixtures.inventory('portable-core').tests
  end, false)
end)

group(function()
  check('windows-v1-full', function(document) table.remove(document.tests, 1) end, false, 'Missing case ID 01')
  check('windows-v1-full', function(document) document.tests[2].name = document.tests[1].name end, false, 'Duplicate case ID 01')
  check('portable-core', function(document) document.tests[6].name = 'avb0795 V1 case 06: fixture' end, false)
  check('windows-v1-full', function(document) document.tests[1].name = 'avb0795 V1 case 99: fixture' end, false)
end)

group(function()
  check('windows-v1-full', function(document) document.tests[1].name = 'avb0795 v1 CASE 01: fixture' end, true)
  check('windows-v1-full', function(document) document.tests[1].name = 'avb0795 V1 case 1: fixture' end, false)
  check('windows-v1-full', function(document) document.tests[1] = json.null end, false)
end)

group(function()
  for _, bad in ipairs({ json.null, fixtures.object(), 3 }) do
    check('windows-v1-full', function(document) document.tests = bad end, false)
  end
  check('windows-v1-full', function(document) document.version.major = 2 end, false)
  check('windows-v1-full', function(document) document.version.minor = -1 end, false)
  check('windows-v1-full', function(document) document.kind = 'wrong' end, false)
  check('windows-v1-full', function(document) document.tests[1].properties = fixtures.object() end, false)
end)

group(function()
  for _, bad in ipairs({ fixtures.array(), fixtures.object(), 'command', json.null }) do
    check('windows-v1-full', function(document) document.tests[1].command = bad end, false)
  end
  check('windows-v1-full', function(document) document.tests[1].command[2] = ' \t\r\n\f\v' end, false)
  check('windows-v1-full', function(document) document.tests[1].command[3] = '\194\160' end, true)
  check('windows-v1-full', function(document)
    document.tests[1].command[1] = 'C:\\測試 路徑\\日本語 & tool.exe'
  end, true)
end)

group(function()
  check('windows-v1-full', function(document) document.tests[1].properties[1].name = 'OTHER' end, false)
  check('windows-v1-full', function(document)
    document.tests[1].properties[2] = fixtures.object({ name = 'LABELS', value = fixtures.array({ 'avb0795-v1' }) })
  end, false)
  check('windows-v1-full', function(document) document.tests[1].properties[1].value = 'avb0795-v1' end, false)
  check('windows-v1-full', function(document) document.tests[1].properties[1].value[1] = 'AVB0795-V1' end, false)
  check('windows-v1-full', function(document) document.tests[1].properties[1].value[2] = 'hardware' end, false)
  check('windows-v1-full', function(document) table.remove(document.tests[13].properties[1].value, 2) end, false)
  check('windows-v1-full', function(document) document.tests[1].properties[1].value[2] = '\227\128\128' end, true)
end)

assert(groups == 7)
print(string.format('POLICY_OK %d groups', groups))
