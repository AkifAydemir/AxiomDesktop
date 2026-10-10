// Convert an existing SVG mark, without redesigning it, to a multi-resolution ICO.
// Requires Node.js and sharp (build-time only). Usage:
// node tools/generate-icon.cjs docs/assets/project-mark.svg path/to/project.ico
const fs = require('node:fs/promises');
const path = require('node:path');
const sharp = require('sharp');
async function main() {
  const [input, output] = process.argv.slice(2);
  if (!input || !output || !output.endsWith('.ico')) throw Error('Expected input.svg output.ico');
  const sizes = [16, 24, 32, 48, 64, 128, 256];
  const svg = await fs.readFile(input);
  const frames = await Promise.all(sizes.map(size =>
    sharp(svg, { density: 384 }).resize(size, size).png().toBuffer()));
  const header = Buffer.alloc(6 + 16 * sizes.length);
  header.writeUInt16LE(1, 2);
  header.writeUInt16LE(sizes.length, 4);
  let offset = header.length;
  frames.forEach((frame, i) => {
    const entry = 6 + 16 * i;
    header[entry] = sizes[i] === 256 ? 0 : sizes[i];
    header[entry + 1] = header[entry];
    header.writeUInt16LE(1, entry + 4);
    header.writeUInt16LE(32, entry + 6);
    header.writeUInt32LE(frame.length, entry + 8);
    header.writeUInt32LE(offset, entry + 12);
    offset += frame.length;
  });
  await fs.mkdir(path.dirname(output), { recursive: true });
  await fs.writeFile(output, Buffer.concat([header, ...frames]));
  console.log(output + ': ' + sizes.join(', ') + ' px');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
