package.path = './vendor/?.lua;../tests/tooling/?.lua'
package.cpath = ''

local json = require('dkjson')
local fixtures = require('inventory_fixtures')

local mode = arg[1] or 'valid'
if mode == 'empty' then
  return
elseif mode == 'malformed' then
  io.write('{')
  return
elseif mode == 'oversized' then
  io.write(string.rep(' ', 4 * 1024 * 1024 + 1))
  return
end

local document = fixtures.inventory(arg[2] or 'windows-v1-full')
if mode == 'wrong-envelope' then
  document.version.major = 2
elseif mode == 'null-entry' then
  document.tests[1] = json.null
end

io.write(assert(json.encode(document)))
if mode == 'trailing' then
  io.write(' false')
end
