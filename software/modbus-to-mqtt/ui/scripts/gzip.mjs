import { createGzip } from 'zlib';
import { createReadStream, createWriteStream } from 'fs';
import { statSync, rmSync } from 'fs';

const [,, src, dest] = process.argv;
if (!src || !dest) {
    console.error('Usage: node gzip.mjs <src> <dest>');
    process.exit(1);
}
try { rmSync(dest); } catch {}
const gz = createGzip({ level: 9 });
createReadStream(src).pipe(gz).pipe(createWriteStream(dest)).on('finish', () => {
    const s = statSync(dest);
    console.log(`gzipped -> ${dest} (${Math.round(s.size/1024)} KB)`);
});
