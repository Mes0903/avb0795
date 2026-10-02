local fixtures = {}

local function object(value)
  return setmetatable(value or {}, { __jsontype = 'object' })
end

local function array(value)
  return setmetatable(value or {}, { __jsontype = 'array' })
end

local ids = {
  ['windows-v1-full'] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
  ['windows-policy'] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 },
  ['portable-core'] = { 1, 2, 3, 4, 5, 7 },
}

fixtures.object = object
fixtures.array = array

function fixtures.inventory(name)
  local tests = array()
  for _, id in ipairs(assert(ids[name], 'unknown fixture profile')) do
    local labels = array({ 'avb0795-v1' })
    if name == 'windows-v1-full' and id >= 13 then
      labels[#labels + 1] = 'hardware'
    end
    tests[#tests + 1] = object({
      name = string.format('avb0795 V1 case %02d: fixture', id),
      command = array({ 'opaque-executable', 'argument' }),
      properties = array({ object({ name = 'LABELS', value = labels }) }),
    })
  end

  return object({
    kind = 'ctestInfo',
    version = object({ major = 1, minor = 0 }),
    tests = tests,
  })
end

return fixtures
