import type { ComponentType, SVGProps } from 'react';
import { Folder, Link2 } from 'lucide-react';
import { Icon } from '../../components/Icon';
import type { FileEntry } from '../../types';
import { fileIconKey, type FileIconKey } from './fileUtils';

// File-type artwork is the MIT-licensed VS Code Icons set, compiled on demand
// by unplugin-icons (see ADR-0007). Only the icons listed here ship.
import GoIcon from '~icons/vscode-icons/file-type-go';
import CIcon from '~icons/vscode-icons/file-type-c';
import CppIcon from '~icons/vscode-icons/file-type-cpp';
import RustIcon from '~icons/vscode-icons/file-type-rust';
import PythonIcon from '~icons/vscode-icons/file-type-python';
import JavaIcon from '~icons/vscode-icons/file-type-java';
import KotlinIcon from '~icons/vscode-icons/file-type-kotlin';
import ScalaIcon from '~icons/vscode-icons/file-type-scala';
import GroovyIcon from '~icons/vscode-icons/file-type-groovy';
import JavascriptIcon from '~icons/vscode-icons/file-type-js';
import TypescriptIcon from '~icons/vscode-icons/file-type-typescript';
import ReactJsIcon from '~icons/vscode-icons/file-type-reactjs';
import ReactTsIcon from '~icons/vscode-icons/file-type-reactts';
import VueIcon from '~icons/vscode-icons/file-type-vue';
import SvelteIcon from '~icons/vscode-icons/file-type-svelte';
import RubyIcon from '~icons/vscode-icons/file-type-ruby';
import PhpIcon from '~icons/vscode-icons/file-type-php';
import SwiftIcon from '~icons/vscode-icons/file-type-swift';
import DartIcon from '~icons/vscode-icons/file-type-dartlang';
import LuaIcon from '~icons/vscode-icons/file-type-lua';
import PerlIcon from '~icons/vscode-icons/file-type-perl';
import RIcon from '~icons/vscode-icons/file-type-r';
import JuliaIcon from '~icons/vscode-icons/file-type-julia';
import HaskellIcon from '~icons/vscode-icons/file-type-haskell';
import ElixirIcon from '~icons/vscode-icons/file-type-elixir';
import ErlangIcon from '~icons/vscode-icons/file-type-erlang';
import ClojureIcon from '~icons/vscode-icons/file-type-clojure';
import FsharpIcon from '~icons/vscode-icons/file-type-fsharp';
import NimIcon from '~icons/vscode-icons/file-type-nim';
import ZigIcon from '~icons/vscode-icons/file-type-zig';
import FortranIcon from '~icons/vscode-icons/file-type-fortran';
import AssemblyIcon from '~icons/vscode-icons/file-type-assembly';
import VbIcon from '~icons/vscode-icons/file-type-vb';
import SqlIcon from '~icons/vscode-icons/file-type-sql';
import SqliteIcon from '~icons/vscode-icons/file-type-sqlite';
import GraphqlIcon from '~icons/vscode-icons/file-type-graphql';
import ProtobufIcon from '~icons/vscode-icons/file-type-protobuf';
import SolidityIcon from '~icons/vscode-icons/file-type-solidity';
import JupyterIcon from '~icons/vscode-icons/file-type-jupyter';
import ShellIcon from '~icons/vscode-icons/file-type-shell';
import PowershellIcon from '~icons/vscode-icons/file-type-powershell';
import BatIcon from '~icons/vscode-icons/file-type-bat';
import MarkdownIcon from '~icons/vscode-icons/file-type-markdown';
import HtmlIcon from '~icons/vscode-icons/file-type-html';
import CssIcon from '~icons/vscode-icons/file-type-css';
import ScssIcon from '~icons/vscode-icons/file-type-scss';
import LessIcon from '~icons/vscode-icons/file-type-less';
import SassIcon from '~icons/vscode-icons/file-type-sass';
import XmlIcon from '~icons/vscode-icons/file-type-xml';
import JsonIcon from '~icons/vscode-icons/file-type-json';
import YamlIcon from '~icons/vscode-icons/file-type-yaml';
import TomlIcon from '~icons/vscode-icons/file-type-toml';
import IniIcon from '~icons/vscode-icons/file-type-ini';
import ConfigIcon from '~icons/vscode-icons/file-type-config';
import DotenvIcon from '~icons/vscode-icons/file-type-dotenv';
import TextIcon from '~icons/vscode-icons/file-type-text';
import LogIcon from '~icons/vscode-icons/file-type-log';
import PdfIcon from '~icons/vscode-icons/file-type-pdf';
import WordIcon from '~icons/vscode-icons/file-type-word';
import ExcelIcon from '~icons/vscode-icons/file-type-excel';
import CsvIcon from '~icons/vscode-icons/file-type-csv';
import PowerpointIcon from '~icons/vscode-icons/file-type-powerpoint';
import ImageIcon from '~icons/vscode-icons/file-type-image';
import AudioIcon from '~icons/vscode-icons/file-type-audio';
import VideoIcon from '~icons/vscode-icons/file-type-video';
import ZipIcon from '~icons/vscode-icons/file-type-zip';
import FontIcon from '~icons/vscode-icons/file-type-font';
import BinaryIcon from '~icons/vscode-icons/file-type-binary';
import DockerIcon from '~icons/vscode-icons/file-type-docker';
import MakefileIcon from '~icons/vscode-icons/file-type-makefile';
import CmakeIcon from '~icons/vscode-icons/file-type-cmake';
import GradleIcon from '~icons/vscode-icons/file-type-gradle';
import MavenIcon from '~icons/vscode-icons/file-type-maven';
import LicenseIcon from '~icons/vscode-icons/file-type-license';
import GitIcon from '~icons/vscode-icons/file-type-git';
import NpmIcon from '~icons/vscode-icons/file-type-npm';
import YarnIcon from '~icons/vscode-icons/file-type-yarn';
import PnpmIcon from '~icons/vscode-icons/file-type-pnpm';
import EslintIcon from '~icons/vscode-icons/file-type-eslint';
import PrettierIcon from '~icons/vscode-icons/file-type-prettier';
import EditorconfigIcon from '~icons/vscode-icons/file-type-editorconfig';
import TsconfigIcon from '~icons/vscode-icons/file-type-tsconfig';
import ViteIcon from '~icons/vscode-icons/file-type-vite';
import VitestIcon from '~icons/vscode-icons/file-type-vitest';
import TerraformIcon from '~icons/vscode-icons/file-type-terraform';
import DefaultFileIcon from '~icons/vscode-icons/default-file';

type Glyph = ComponentType<SVGProps<SVGSVGElement>>;

const FILE_ICONS: Record<FileIconKey, Glyph> = {
  go: GoIcon,
  c: CIcon,
  cpp: CppIcon,
  rust: RustIcon,
  python: PythonIcon,
  java: JavaIcon,
  kotlin: KotlinIcon,
  scala: ScalaIcon,
  groovy: GroovyIcon,
  javascript: JavascriptIcon,
  typescript: TypescriptIcon,
  reactjs: ReactJsIcon,
  reactts: ReactTsIcon,
  vue: VueIcon,
  svelte: SvelteIcon,
  ruby: RubyIcon,
  php: PhpIcon,
  swift: SwiftIcon,
  dartlang: DartIcon,
  lua: LuaIcon,
  perl: PerlIcon,
  r: RIcon,
  julia: JuliaIcon,
  haskell: HaskellIcon,
  elixir: ElixirIcon,
  erlang: ErlangIcon,
  clojure: ClojureIcon,
  fsharp: FsharpIcon,
  nim: NimIcon,
  zig: ZigIcon,
  fortran: FortranIcon,
  assembly: AssemblyIcon,
  vb: VbIcon,
  sql: SqlIcon,
  sqlite: SqliteIcon,
  graphql: GraphqlIcon,
  protobuf: ProtobufIcon,
  solidity: SolidityIcon,
  jupyter: JupyterIcon,
  shell: ShellIcon,
  powershell: PowershellIcon,
  bat: BatIcon,
  markdown: MarkdownIcon,
  html: HtmlIcon,
  css: CssIcon,
  scss: ScssIcon,
  less: LessIcon,
  sass: SassIcon,
  xml: XmlIcon,
  json: JsonIcon,
  yaml: YamlIcon,
  toml: TomlIcon,
  ini: IniIcon,
  config: ConfigIcon,
  dotenv: DotenvIcon,
  text: TextIcon,
  log: LogIcon,
  pdf: PdfIcon,
  word: WordIcon,
  excel: ExcelIcon,
  csv: CsvIcon,
  powerpoint: PowerpointIcon,
  image: ImageIcon,
  audio: AudioIcon,
  video: VideoIcon,
  zip: ZipIcon,
  font: FontIcon,
  binary: BinaryIcon,
  docker: DockerIcon,
  makefile: MakefileIcon,
  cmake: CmakeIcon,
  gradle: GradleIcon,
  maven: MavenIcon,
  license: LicenseIcon,
  git: GitIcon,
  npm: NpmIcon,
  yarn: YarnIcon,
  pnpm: PnpmIcon,
  eslint: EslintIcon,
  prettier: PrettierIcon,
  editorconfig: EditorconfigIcon,
  tsconfig: TsconfigIcon,
  vite: ViteIcon,
  vitest: VitestIcon,
  terraform: TerraformIcon,
  default: DefaultFileIcon,
};

type Props = {
  entry: FileEntry;
  size?: number;
  className?: string;
};

/**
 * Icon for a listing entry: VS Code file-type artwork for files, Lucide for
 * directories and symlinks (whose form is generic rather than format-specific).
 */
export function EntryIcon({ entry, size = 16, className }: Props) {
  if (entry.type === 'symlink') return <Icon icon={Link2} size={size} className={className} />;
  if (entry.type === 'dir') return <Icon icon={Folder} size={size} className={className} />;
  const Glyph = FILE_ICONS[fileIconKey(entry)];
  return (
    <Glyph
      width={size}
      height={size}
      className={className}
      aria-hidden="true"
      focusable="false"
    />
  );
}
