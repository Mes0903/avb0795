-- Internal stdin-only adapter for the public CMake inventory launcher.
-- It owns module setup, JSON decoding, policy diagnostics and exit categories:
-- 0 accepts the chosen metadata profile, 1 rejects input/policy, and 2 reports
-- invocation/setup/internal failure. Only the outer launcher can also attest the
-- CTest producer's exit status; running this CLI alone cannot establish that fact.
-- Invoke from tools/ with Lua -E so local module resolution is deterministic.
local function write_error(message)
  assert(io.stderr:write(message, '\n'))
end

local function usage()
  return 'Usage: lua -E verify_test_inventory.lua [--profile PROFILE]'
end

-- Keep setup and execution inside one protected boundary so missing modules,
-- decoding failures and checked output errors cannot escape as false success.
-- Loading only the local pure-Lua modules avoids adding a native module search
-- path; this narrows dependency resolution but does not sandbox the input producer.
local function run()
  if _VERSION ~= 'Lua 5.4' and _VERSION ~= 'Lua 5.5' then
    write_error('Unsupported Lua runtime: expected Lua 5.4 or Lua 5.5, got ' .. tostring(_VERSION))
    return 2
  end

  package.path = './lib/?.lua;./vendor/?.lua'
  package.cpath = ''

  -- Local modules are loaded inside run so setup failures share the outer guard.
  local profiles = require('inventory_profiles')
  local policy = require('test_inventory')
  local json = require('dkjson')

  if #arg == 1 and arg[1] == '--help' then
    assert(io.write(usage(), '\n'))
    assert(io.flush())
    return 0
  end

  local profile_name = 'windows-v1-full'
  if #arg ~= 0 then
    if #arg ~= 2 or arg[1] ~= '--profile'
        or type(arg[2]) ~= 'string' or arg[2] == '' then
      write_error(usage())
      return 2
    end
    profile_name = arg[2]
  end

  local profile = profiles[profile_name]
  if profile == nil then
    write_error('Unknown inventory profile: ' .. profile_name)
    return 2
  end

  -- The limit applies to text delivered by Lua, rather than raw operating-system bytes.
  local limit = 4 * 1024 * 1024
  local text, read_error = io.read(limit + 1)
  if text == nil then
    if read_error ~= nil then
      write_error('Unable to read CTest inventory: ' .. tostring(read_error))
      return 2
    end
    text = ''
  end
  if #text > limit then
    write_error('CTest inventory exceeds the 4 MiB limit')
    return 1
  end
  if text:gsub('[ \t\r\n\f\v]', '') == '' then
    write_error('CTest inventory is empty')
    return 1
  end

  -- Preserve JSON null and object/array identity for the pure validator. A decoded
  -- prefix is insufficient: require a sensible returned position and only allowed
  -- trailing whitespace in the text Lua actually delivered. This is not a claim
  -- about raw bytes hidden by a platform's text-mode stdin translation.
  local decode_ok, document, position, decode_error = pcall(json.decode, text, 1, json.null)
  if not decode_ok then
    write_error('Invalid CTest inventory JSON: ' .. tostring(document))
    return 1
  end
  if decode_error ~= nil or type(position) ~= 'number'
      or position % 1 ~= 0 or position < 1 or position > #text + 1 then
    write_error('Invalid CTest inventory JSON: ' .. tostring(decode_error or 'invalid decoder position'))
    return 1
  end
  if text:sub(position):find('[^ \t\r\n]') then
    write_error('Invalid CTest inventory JSON: trailing content')
    return 1
  end

  local errors = policy.validate_inventory(document, profile)
  if #errors ~= 0 then
    write_error('CTest inventory validation failed:')
    for _, diagnostic in ipairs(errors) do
      write_error(diagnostic)
    end
    return 1
  end

  if profile_name == 'windows-v1-full' then
    assert(io.write('CTest inventory is valid: 15 avb0795 V1 cases; hardware cases 13-15.\n'))
  else
    assert(io.write(string.format(
      'CTest inventory is valid: profile %s; %d avb0795 V1 cases; no hardware cases.\n',
      profile_name, #profile.ids)))
  end
  assert(io.flush())
  return 0
end

-- A bounded fallback diagnostic handles unexpected adapter failures. Closing the
-- Lua state at exit releases interpreter resources; it neither executes inventory
-- commands nor changes test metadata in the configured build tree.
local ok, code = pcall(run)
if not ok then
  io.stderr:write('Inventory tool setup/internal failure: ', tostring(code):sub(1, 300), '\n')
  code = 2
end
os.exit(code, true)
