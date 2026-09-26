const fs = require('node:fs');
const { createRequire } = require('node:module');
const path = require('node:path');
const localRequire = createRequire(path.resolve(__dirname, '../../apps/dashboard/package.json'));
const YAML = localRequire('yaml');
const config = YAML.parse(fs.readFileSync(process.argv[2], 'utf8'));
if (!config.control?.token) process.exit(1);
process.stdout.write(config.control.token);
