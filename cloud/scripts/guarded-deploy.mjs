import { execFileSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const videoRoot = join(root, '..', 'video');
const deployScript = join(root, 'scripts', 'deploy-existing.mjs');
const wranglerCli = join(root, 'node_modules', 'wrangler', 'bin', 'wrangler.js');
const generatedConfig = join(root, '.wrangler', 'generated', 'homepanel-existing.jsonc');
const videoDependencyMarker = join(videoRoot, 'node_modules', '@cloudflare', 'puppeteer', 'package.json');
const productionBranch = process.env.HOMEPANEL_PRODUCTION_BRANCH?.trim() || 'main';
const buildBranch = process.env.WORKERS_CI_BRANCH?.trim() || '';
const cloudflareManagedBuild = process.env.WORKERS_CI === '1';
const previewBuild = cloudflareManagedBuild && Boolean(buildBranch) && buildBranch !== productionBranch;
const allowInactiveDeploy = process.env.HOMEPANEL_ALLOW_INACTIVE_VIDEO_DEPLOY?.trim() === '1';

function runNode(args, capture = false) {
  return execFileSync(process.execPath, args, {
    cwd: root,
    env: { ...process.env, CI: 'true' },
    encoding: capture ? 'utf8' : undefined,
    stdio: capture ? ['ignore', 'pipe', 'pipe'] : 'inherit'
  });
}

function ensureVideoDependencies() {
  if (existsSync(videoDependencyMarker)) return;
  const npmCommand = process.platform === 'win32' ? 'npm.cmd' : 'npm';
  console.log('Installing imported video Worker dependencies from package-lock.json before bundling.');
  execFileSync(npmCommand, ['--prefix', videoRoot, 'ci', '--no-audit', '--no-fund'], {
    cwd: root,
    env: { ...process.env, CI: 'true' },
    stdio: 'inherit'
  });
  if (!existsSync(videoDependencyMarker)) {
    throw new Error('The imported video Worker dependency @cloudflare/puppeteer was not installed');
  }
}

function activationIsComplete() {
  try {
    runNode([deployScript, '--prepare-only']);
    const output = runNode([
      wranglerCli,
      'd1',
      'execute',
      'homepanel-data',
      '--remote',
      '--config',
      generatedConfig,
      '--command',
      'SELECT active FROM video_runtime_state WHERE id=1;',
      '--json'
    ], true);
    return /"active"\s*:\s*1/.test(output);
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    console.log(`Unified video runtime activation could not be verified: ${message}`);
    return false;
  }
}

if (cloudflareManagedBuild && !previewBuild && !allowInactiveDeploy && !activationIsComplete()) {
  console.log('Skipping the Cloudflare-managed production deploy until the video runtime activation marker is active.');
  console.log('The existing homepanel-cloud Worker remains unchanged.');
  process.exit(0);
}

ensureVideoDependencies();
runNode([deployScript, ...process.argv.slice(2)]);
