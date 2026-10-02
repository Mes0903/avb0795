-- Pure validation of an already-decoded CTest inventory against a trusted profile.
-- This module returns ordered English diagnostics and never reads files, executes
-- command entries, selects a host platform, or decides the caller's exit status.
-- Decoder-owned object/array tags are part of its input contract, including for
-- empty containers; callers must not replace JSON shape with plain Lua tables.
local policy = {}

-- JSON containers are intentionally distinguished by their decoder-owned tags;
-- a plain Lua table is not interchangeable with either representation.
local function has_json_type(value, expected)
  if type(value) ~= 'table' then
    return false
  end
  local metatable = getmetatable(value)
  return metatable ~= nil and metatable.__jsontype == expected
end

local function ascii_lower(value)
  return (value:gsub('[A-Z]', function(character)
    return string.char(string.byte(character) + 32)
  end))
end

local function is_nonblank_string(value)
  return type(value) == 'string' and value:gsub('[ \t\r\n\f\v]', '') ~= ''
end

local function append(errors, message)
  errors[#errors + 1] = message
end

local function requires_hardware(profile, id)
  for _, hardware_id in ipairs(profile.hardware) do
    if hardware_id == id then
      return true
    end
  end
  return false
end

-- Inspect only the array's shape and nonblank argument values. Commands remain
-- opaque data: existence, launchability and success cannot be inferred here.
-- Accumulate independent errors so one invocation can explain multiple defects.
local function validate_command(command, test_index, errors)
  if not has_json_type(command, 'array') then
    append(errors, string.format('Test %d: command must be a JSON array', test_index))
    return
  end
  if #command == 0 then
    append(errors, string.format('Test %d: command must not be empty', test_index))
    return
  end
  for argument_index, argument in ipairs(command) do
    if not is_nonblank_string(argument) then
      append(errors, string.format('Test %d: command argument %d must be a nonblank string',
        test_index, argument_index))
    end
  end
end

-- One LABELS property supplies both membership and hardware classification.
-- Required labels use exact spelling, while unrelated nonblank labels may remain.
-- A malformed/unknown case ID cannot define a hardware expectation; report its
-- name error elsewhere and avoid manufacturing an additional ID-specific claim.
local function validate_labels(properties, test_index, expected_id, profile, errors)
  if not has_json_type(properties, 'array') then
    append(errors, string.format('Test %d: properties must be a JSON array', test_index))
    return
  end

  local labels_property
  local labels_count = 0
  for property_index, property in ipairs(properties) do
    if not has_json_type(property, 'object') then
      append(errors, string.format('Test %d: property %d must be an object', test_index, property_index))
    elseif property.name == 'LABELS' then
      labels_count = labels_count + 1
      labels_property = property
    end
  end

  if labels_count ~= 1 then
    append(errors, string.format('Test %d: expected exactly one LABELS property', test_index))
    return
  end
  if not has_json_type(labels_property.value, 'array') then
    append(errors, string.format('Test %d: LABELS value must be a JSON array', test_index))
    return
  end

  local has_required_label = false
  local has_hardware = false
  local labels_valid = true
  for label_index, label in ipairs(labels_property.value) do
    if not is_nonblank_string(label) then
      append(errors, string.format('Test %d: label %d must be a nonblank string', test_index, label_index))
      labels_valid = false
    else
      has_required_label = has_required_label or label == profile.required_label
      has_hardware = has_hardware or label == 'hardware'
    end
  end

  if labels_valid and not has_required_label then
    append(errors, string.format('Test %d: missing required label %s', test_index, profile.required_label))
  end
  if labels_valid and expected_id ~= nil and has_hardware ~= requires_hardware(profile, expected_id) then
    append(errors, string.format('Test %d: hardware label does not match case ID %02d', test_index, expected_id))
  end
end

-- Validate envelope, exact case inventory, command metadata and label contracts.
-- document must retain the decoder's JSON tags; profile is trusted module data,
-- not user JSON. Neither input is mutated. The returned fresh diagnostic array
-- is empty exactly when these metadata rules pass, not when tests have passed.
-- Input-order errors precede missing IDs in profile order for reproducible output.
-- Bad root/tests shape ends that branch early; independent per-test faults are
-- collected rather than stopping at the first invalid field.
function policy.validate_inventory(document, profile)
  local errors = {}
  if not has_json_type(document, 'object') then
    append(errors, 'Inventory root must be an object')
    return errors
  end

  local version = document.version
  local envelope_valid = document.kind == 'ctestInfo'
    and has_json_type(version, 'object')
    and version.major == 1
    and type(version.minor) == 'number'
    and version.minor >= 0
    and version.minor % 1 == 0
  if not envelope_valid then
    append(errors, 'Unsupported CTest JSON envelope')
  end

  local tests = document.tests
  if not has_json_type(tests, 'array') then
    append(errors, 'tests must be a JSON array')
    return errors
  end
  if #tests ~= #profile.ids then
    append(errors, string.format('Expected exactly %d discovered tests; found %d', #profile.ids, #tests))
  end

  local expected = {}
  for _, id in ipairs(profile.ids) do
    expected[id] = true
  end
  local seen = {}

  for test_index, test in ipairs(tests) do
    if not has_json_type(test, 'object') then
      append(errors, string.format('Test %d: entry must be an object', test_index))
    else
      local id
      if not is_nonblank_string(test.name) then
        append(errors, string.format('Test %d: name must be a nonblank string', test_index))
      else
        local digits = ascii_lower(test.name):match('^avb0795 v1 case ([0-9][0-9]): .+$')
        if digits == nil then
          append(errors, string.format('Test %d: name does not match the avb0795 V1 case format', test_index))
        else
          id = tonumber(digits)
          if seen[id] then
            append(errors, string.format('Duplicate case ID %02d', id))
          else
            seen[id] = true
          end
          if not expected[id] then
            append(errors, string.format('Test %d: unexpected case ID %02d', test_index, id))
            id = nil
          end
        end
      end

      validate_command(test.command, test_index, errors)
      validate_labels(test.properties, test_index, id, profile, errors)
    end
  end

  -- Missing IDs follow trusted profile order, independent of input ordering.
  for _, id in ipairs(profile.ids) do
    if not seen[id] then
      append(errors, string.format('Missing case ID %02d', id))
    end
  end

  return errors
end

return policy
