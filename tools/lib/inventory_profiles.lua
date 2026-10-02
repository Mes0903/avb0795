-- Trusted inventory contracts, not platform detection or executable instructions.
-- Each named profile fixes the exact expected IDs and their hardware-label set;
-- input metadata cannot redefine them. Smaller profiles are explicit development
-- scopes, never automatic fallbacks from the default full Windows inventory.
-- Consumers borrow these tables and must not mutate them. No I/O or host query
-- occurs here, so the same policy data is usable by the CLI and fixture checks.
local profiles = {
  ['windows-v1-full'] = {
    name = 'windows-v1-full',
    ids = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
    hardware = { 13, 14, 15 },
    required_label = 'avb0795-v1',
  },
  ['windows-policy'] = {
    name = 'windows-policy',
    ids = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 },
    hardware = {},
    required_label = 'avb0795-v1',
  },
  ['portable-core'] = {
    name = 'portable-core',
    ids = { 1, 2, 3, 4, 5, 7 },
    hardware = {},
    required_label = 'avb0795-v1',
  },
}

return profiles
