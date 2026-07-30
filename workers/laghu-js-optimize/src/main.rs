// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

use std::{
    fs::{self, File, OpenOptions},
    io::{self, Read, Write},
    path::{Path, PathBuf},
    thread,
    time::{Duration, SystemTime, UNIX_EPOCH},
};

use anyhow::{Context, Result, anyhow, bail};
use browserslist::{Opts, resolve};
use fs2::FileExt;
use memmap2::{MmapMut, MmapOptions};
use sha2::{Digest, Sha256};
use swc_core::{
    common::{
        FileName, GLOBALS, Globals, Mark, SourceMap, comments::SingleThreadedComments, sync::Lrc,
    },
    ecma::{
        ast::{
            Callee, EsVersion, ExportAll, Ident, ImportDecl, MemberExpr, MetaPropExpr,
            MetaPropKind, NamedExport, Program, Script, Stmt,
        },
        codegen::{Config as CodegenConfig, Emitter, text_writer::JsWriter},
        minifier::{
            optimize,
            option::{CompressOptions, ExtraOptions, MangleOptions, MinifyOptions},
        },
        parser::{EsSyntax, Parser, StringInput, Syntax, lexer::Lexer},
        preset_env::{Config as EnvConfig, EnvConfig as ResolvedEnv, Targets, transform_from_env},
        transforms::base::{assumptions::Assumptions, fixer::fixer, hygiene::hygiene, resolver},
        visit::{Visit, VisitWith},
    },
};

#[cfg(windows)]
use std::sync::{
    Arc,
    atomic::{AtomicBool, Ordering},
};

const MAX_INPUT: usize = 2 * 1024 * 1024;
const DEFAULT_TARGET: &str = "defaults and supports es6-module and not dead";
const QUEUE_MAGIC: u64 = 0x4c41_4748_5551_5545;
const QUEUE_VERSION: u32 = 7;
const CACHE_MAGIC: u64 = 0x4c41_4748_5543_4143;
const CACHE_VERSION: u32 = 2;
const JOB_JAVASCRIPT: u32 = 3;
const SLOT_READY: u32 = 1;
const HEADER_SIZE: usize = 176;
const SLOT_SIZE: usize = 4_544;
const BACKEND: &str = "laghu-swc-75.0.0-js-v1";
const CATALOG_MAGIC: u64 = 0x4c41_4748_554a_5343;
const CATALOG_SIZE: usize = 1_908;
const FLAG_MODULE: u32 = 1;
const FLAG_URL_INDEPENDENT: u32 = 2;
const FLAG_CONCAT_SAFE: u32 = 4;
const FLAG_TOP_LEVEL_DECLARATION_FREE: u32 = 8;

#[derive(Default)]
struct Eligibility {
    url_independent: bool,
    concat_safe: bool,
}

impl Visit for Eligibility {
    fn visit_ident(&mut self, node: &Ident) {
        if matches!(&*node.sym, "eval" | "Function" | "currentScript") {
            self.url_independent = false;
            self.concat_safe = false;
        }
    }

    fn visit_member_expr(&mut self, node: &MemberExpr) {
        if node.prop.is_ident_with("currentScript") {
            self.url_independent = false;
            self.concat_safe = false;
        }
        node.visit_children_with(self);
    }

    fn visit_call_expr(&mut self, node: &swc_core::ecma::ast::CallExpr) {
        if matches!(node.callee, Callee::Import(_)) {
            self.url_independent = false;
            self.concat_safe = false;
        }
        node.visit_children_with(self);
    }

    fn visit_meta_prop_expr(&mut self, node: &MetaPropExpr) {
        if node.kind == MetaPropKind::ImportMeta {
            self.url_independent = false;
            self.concat_safe = false;
        }
    }

    fn visit_import_decl(&mut self, node: &ImportDecl) {
        if node.src.value.to_string_lossy().starts_with('.') {
            self.url_independent = false;
        }
        node.visit_children_with(self);
    }

    fn visit_named_export(&mut self, node: &NamedExport) {
        if node
            .src
            .as_ref()
            .is_some_and(|src| src.value.to_string_lossy().starts_with('.'))
        {
            self.url_independent = false;
        }
        node.visit_children_with(self);
    }

    fn visit_export_all(&mut self, node: &ExportAll) {
        if node.src.value.to_string_lossy().starts_with('.') {
            self.url_independent = false;
        }
        node.visit_children_with(self);
    }
}

fn eligibility(program: &Program, module: bool) -> u32 {
    let top_level_declaration_free = !module
        && matches!(program, Program::Script(Script { body, .. }) if body.iter().all(|statement| matches!(statement, Stmt::Expr(_) | Stmt::Empty(_))));
    let mut result = Eligibility {
        url_independent: true,
        concat_safe: top_level_declaration_free,
    };
    program.visit_with(&mut result);
    (u32::from(module) * FLAG_MODULE)
        | (u32::from(result.url_independent) * FLAG_URL_INDEPENDENT)
        | (u32::from(result.concat_safe) * FLAG_CONCAT_SAFE)
        | (u32::from(top_level_declaration_free) * FLAG_TOP_LEVEL_DECLARATION_FREE)
}

fn c_string(bytes: &[u8]) -> Result<&str> {
    let end = bytes
        .iter()
        .position(|byte| *byte == 0)
        .context("unterminated queue string")?;
    std::str::from_utf8(&bytes[..end]).context("queue string is not UTF-8")
}

fn read_u32(bytes: &[u8], offset: usize) -> u32 {
    u32::from_ne_bytes(
        bytes[offset..offset + 4]
            .try_into()
            .expect("fixed queue layout"),
    )
}

fn read_u64(bytes: &[u8], offset: usize) -> u64 {
    u64::from_ne_bytes(
        bytes[offset..offset + 8]
            .try_into()
            .expect("fixed queue layout"),
    )
}

fn write_u32(bytes: &mut [u8], offset: usize, value: u32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_ne_bytes());
}

fn write_u64(bytes: &mut [u8], offset: usize, value: u64) {
    bytes[offset..offset + 8].copy_from_slice(&value.to_ne_bytes());
}

fn sha256(data: &[u8]) -> String {
    format!("{:x}", Sha256::digest(data))
}

fn resolved_target(target: &str) -> Result<String> {
    if target.is_empty()
        || target.len() > 512
        || target.bytes().any(|byte| byte.is_ascii_control())
        || target.contains('$')
        || target.contains("..")
        || target.contains("extends ")
    {
        bail!("invalid javascript target");
    }
    let distributions =
        resolve([target], &Opts::default()).context("unsupported browserslist query")?;
    if distributions.is_empty() {
        bail!("javascript target resolves to an empty browser matrix");
    }
    Ok(distributions
        .iter()
        .map(|distribution| format!("{} {}", distribution.name(), distribution.version()))
        .collect::<Vec<_>>()
        .join(","))
}

fn transform(source: &str, module: bool, target: &str) -> Result<(Vec<u8>, String, u32)> {
    let matrix = resolved_target(target)?;
    GLOBALS.set(&Globals::new(), || {
        let cm: Lrc<SourceMap> = Default::default();
        let comments = SingleThreadedComments::default();
        let file = cm.new_source_file(
            FileName::Custom("input.js".into()).into(),
            source.to_owned(),
        );
        let lexer = Lexer::new(
            Syntax::Es(EsSyntax {
                allow_return_outside_function: !module,
                ..Default::default()
            }),
            EsVersion::latest(),
            StringInput::from(&*file),
            Some(&comments),
        );
        let mut parser = Parser::new_from(lexer);
        let mut program = if module {
            Program::Module(
                parser
                    .parse_module()
                    .map_err(|error| anyhow!("invalid module: {error:?}"))?,
            )
        } else {
            Program::Script(
                parser
                    .parse_script()
                    .map_err(|error| anyhow!("invalid classic script: {error:?}"))?,
            )
        };
        if !parser.take_errors().is_empty() {
            bail!("javascript parser recovery was required");
        }
        let mut flags = eligibility(&program, module);
        if source.contains("sourceMappingURL=") || source.contains("sourceURL=") {
            flags &= !(FLAG_URL_INDEPENDENT | FLAG_CONCAT_SAFE);
        }
        let unresolved = Mark::new();
        let top_level = Mark::new();
        program.mutate(resolver(unresolved, top_level, false));
        let targets: Targets = serde_json::from_value(serde_json::Value::String(target.into()))?;
        program.mutate(transform_from_env(
            unresolved,
            Some(comments.clone()),
            ResolvedEnv::from(EnvConfig {
                targets: Some(targets),
                bugfixes: true,
                ..Default::default()
            }),
            Assumptions::default(),
        ));
        let mangle = MangleOptions {
            top_level: Some(false),
            props: None,
            ..Default::default()
        };
        let compress = CompressOptions {
            unsafe_passes: false,
            ..Default::default()
        };
        let options = MinifyOptions {
            compress: Some(compress),
            mangle: Some(mangle),
            ..Default::default()
        };
        program = optimize(
            program,
            cm.clone(),
            Some(&comments),
            None,
            &options,
            &ExtraOptions {
                unresolved_mark: unresolved,
                top_level_mark: top_level,
                mangle_name_cache: None,
            },
        );
        program.mutate(hygiene());
        program.mutate(fixer(None));
        let mut output = Vec::new();
        {
            let writer = JsWriter::new(cm.clone(), "\n", &mut output, None);
            let mut emitter = Emitter {
                cfg: CodegenConfig::default().with_minify(true),
                comments: Some(&comments),
                cm,
                wr: Box::new(writer),
            };
            emitter.emit_program(&program)?;
        }
        if output.len() >= source.len() || output.len() > MAX_INPUT {
            bail!("javascript output is not smaller");
        }
        Ok((output, matrix, flags))
    })
}

struct Queue {
    file: File,
    map: MmapMut,
    slots: usize,
    payload_size: usize,
}

struct Job {
    index_key: String,
    request_path: String,
    validator: String,
    policy_key: String,
    target: String,
    module: bool,
    source: Vec<u8>,
}

impl Queue {
    fn create(path: &Path) -> Result<Self> {
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent)?;
        }
        let slots = 8usize;
        let length = HEADER_SIZE + slots * (SLOT_SIZE + MAX_INPUT);
        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(true)
            .open(path)?;
        file.set_len(length as u64)?;
        let mut map = unsafe { MmapOptions::new().map_mut(&file)? };
        map.fill(0);
        write_u64(&mut map, 0, QUEUE_MAGIC);
        write_u32(&mut map, 8, QUEUE_VERSION);
        write_u32(&mut map, 12, slots as u32);
        write_u64(&mut map, 16, MAX_INPUT as u64);
        map.flush()?;
        Ok(Self {
            file,
            map,
            slots,
            payload_size: MAX_INPUT,
        })
    }

    fn open(path: &Path) -> Result<Self> {
        let file = OpenOptions::new().read(true).write(true).open(path)?;
        let map = unsafe { MmapOptions::new().map_mut(&file)? };
        if map.len() < HEADER_SIZE
            || read_u64(&map, 0) != QUEUE_MAGIC
            || read_u32(&map, 8) != QUEUE_VERSION
        {
            bail!("incompatible javascript queue");
        }
        let slots = read_u32(&map, 12) as usize;
        let payload_size = usize::try_from(read_u64(&map, 16))?;
        let expected = HEADER_SIZE
            .checked_add(
                slots
                    .checked_mul(
                        SLOT_SIZE
                            .checked_add(payload_size)
                            .context("queue size overflow")?,
                    )
                    .context("queue size overflow")?,
            )
            .context("queue size overflow")?;
        if slots == 0 || payload_size < MAX_INPUT || map.len() != expected {
            bail!("invalid javascript queue dimensions");
        }
        Ok(Self {
            file,
            map,
            slots,
            payload_size,
        })
    }

    fn advertise(&mut self) -> Result<()> {
        self.file.lock_exclusive()?;
        write_u32(&mut self.map, 32, 1);
        write_u64(
            &mut self.map,
            40,
            SystemTime::now().duration_since(UNIX_EPOCH)?.as_secs(),
        );
        self.map[48..176].fill(0);
        self.map[48..48 + BACKEND.len()].copy_from_slice(BACKEND.as_bytes());
        self.map.flush_range(0, HEADER_SIZE)?;
        FileExt::unlock(&self.file)?;
        Ok(())
    }

    fn take(&mut self) -> Result<Option<Job>> {
        if self.file.try_lock_exclusive().is_err() {
            return Ok(None);
        }
        let result = (|| {
            let read = read_u32(&self.map, 28) as usize % self.slots;
            let base = HEADER_SIZE + read * (SLOT_SIZE + self.payload_size);
            if read_u32(&self.map, base) != SLOT_READY {
                return Ok(None);
            }
            let length = usize::try_from(read_u64(&self.map, base + 8))?;
            let valid = read_u32(&self.map, base + 4) == JOB_JAVASCRIPT
                && length > 0
                && length <= MAX_INPUT
                && length <= self.payload_size;
            let job = if valid {
                Some(Job {
                    index_key: c_string(&self.map[base + 16..base + 81])?.to_owned(),
                    request_path: c_string(&self.map[base + 81..base + 1105])?.to_owned(),
                    validator: c_string(&self.map[base + 1105..base + 1361])?.to_owned(),
                    policy_key: c_string(&self.map[base + 1425..base + 1490])?.to_owned(),
                    target: c_string(&self.map[base + 1619..base + 2131])?.to_owned(),
                    module: read_u64(&self.map, base + 2136) & 1 != 0,
                    source: self.map[base + SLOT_SIZE..base + SLOT_SIZE + length].to_vec(),
                })
            } else {
                None
            };
            self.map[base..base + SLOT_SIZE + length.min(self.payload_size)].fill(0);
            write_u32(&mut self.map, 28, ((read + 1) % self.slots) as u32);
            self.map.flush_async()?;
            Ok(job)
        })();
        FileExt::unlock(&self.file)?;
        result
    }
}

fn write_fixed(target: &mut [u8], value: &str) -> Result<()> {
    if value.len() >= target.len() {
        bail!("cache metadata field is too large");
    }
    target.fill(0);
    target[..value.len()].copy_from_slice(value.as_bytes());
    Ok(())
}

fn atomic_write(path: &Path, data: &[u8]) -> Result<()> {
    let temporary = path.with_extension(format!("tmp-{}", std::process::id()));
    {
        let mut file = OpenOptions::new()
            .create(true)
            .truncate(true)
            .write(true)
            .open(&temporary)?;
        file.write_all(data)?;
        file.sync_all()?;
    }
    fs::rename(temporary, path)?;
    Ok(())
}

fn publish(cache: &Path, job: &Job, output: &[u8]) -> Result<()> {
    if job.index_key.len() != 64 || job.validator.len() != 64 {
        bail!("invalid javascript cache key");
    }
    fs::create_dir_all(cache)?;
    let variant = sha256(output);
    atomic_write(&cache.join(format!("variant-{variant}.bin")), output)?;
    let mut metadata = vec![0u8; 608];
    write_u64(&mut metadata, 0, CACHE_MAGIC);
    write_u32(&mut metadata, 8, CACHE_VERSION);
    write_u64(&mut metadata, 16, output.len() as u64);
    write_fixed(&mut metadata[24..89], &variant)?;
    write_fixed(&mut metadata[89..154], &variant)?;
    write_fixed(&mut metadata[154..410], &job.validator)?;
    write_fixed(&mut metadata[410..474], "application/javascript")?;
    write_fixed(&mut metadata[474..602], BACKEND)?;
    atomic_write(
        &cache.join(format!("index-{}.meta", job.index_key)),
        &metadata,
    )?;
    atomic_write(&cache.join(format!("index-{variant}.meta")), &metadata)?;
    Ok(())
}

fn catalog_key(job: &Job) -> String {
    sha256(
        format!(
            "laghu-js-url-v1\n{}\n{}\n{}\n{}",
            job.request_path,
            job.policy_key,
            job.target,
            if job.module { "module" } else { "classic" }
        )
        .as_bytes(),
    )
}

fn publish_catalog(cache: &Path, job: &Job, output: &[u8], matrix: &str, flags: u32) -> Result<()> {
    if !job.request_path.starts_with('/') || job.policy_key.len() != 64 {
        return Ok(());
    }
    let variant = sha256(output);
    let matrix_digest = sha256(matrix.as_bytes());
    atomic_write(
        &cache.join(format!("javascript-matrix-{matrix_digest}.txt")),
        matrix.as_bytes(),
    )?;
    let mut catalog = vec![0u8; CATALOG_SIZE];
    write_u64(&mut catalog, 0, CATALOG_MAGIC);
    write_u32(&mut catalog, 8, 2);
    write_u32(&mut catalog, 12, u32::from(job.module));
    write_u64(
        &mut catalog,
        16,
        SystemTime::now().duration_since(UNIX_EPOCH)?.as_secs(),
    );
    write_u64(&mut catalog, 24, job.source.len() as u64);
    write_u64(&mut catalog, 32, output.len() as u64);
    write_fixed(&mut catalog[40..1064], &job.request_path)?;
    write_fixed(&mut catalog[1064..1129], &job.policy_key)?;
    write_fixed(&mut catalog[1129..1641], &job.target)?;
    write_fixed(&mut catalog[1641..1706], &job.validator)?;
    write_fixed(&mut catalog[1706..1771], &variant)?;
    write_fixed(&mut catalog[1771..1836], &matrix_digest)?;
    write_u32(&mut catalog, 1836, flags);
    let checksum = sha256(&catalog[..1840]);
    write_fixed(&mut catalog[1840..1905], &checksum)?;
    atomic_write(
        &cache.join(format!("javascript-{}.meta", catalog_key(job))),
        &catalog,
    )
}

fn process_one(queue: &mut Queue, cache: &Path) -> Result<bool> {
    queue.advertise()?;
    let Some(job) = queue.take()? else {
        return Ok(false);
    };
    let source = std::str::from_utf8(&job.source).context("javascript source is not UTF-8")?;
    if let Ok((output, matrix, flags)) = transform(source, job.module, &job.target) {
        publish(cache, &job, &output)?;
        publish_catalog(cache, &job, &output, &matrix, flags)?;
    }
    Ok(true)
}

fn queue_args(mut args: impl Iterator<Item = String>) -> Result<(PathBuf, PathBuf)> {
    let queue = PathBuf::from(args.next().context("missing queue path")?);
    let cache = PathBuf::from(args.next().context("missing cache path")?);
    if args.next().is_some() {
        bail!("unexpected worker argument");
    }
    Ok((queue, cache))
}

#[cfg(windows)]
mod windows_worker_service {
    use super::*;
    use std::{ffi::OsString, sync::mpsc};
    use windows_service::{
        define_windows_service,
        service::{
            ServiceControl, ServiceControlAccept, ServiceExitCode, ServiceState, ServiceStatus,
            ServiceType,
        },
        service_control_handler::{self, ServiceControlHandlerResult},
        service_dispatcher,
    };

    const NAME: &str = "laghu-js-optimize";
    define_windows_service!(ffi_service_main, service_main);

    pub fn dispatch() -> Result<()> {
        service_dispatcher::start(NAME, ffi_service_main)?;
        Ok(())
    }

    fn service_main(_arguments: Vec<OsString>) {
        if let Err(error) = run() {
            eprintln!("laghu-js-optimize service failed: {error:#}");
        }
    }

    fn run() -> Result<()> {
        let mut arguments = std::env::args_os().skip(2);
        let queue_path = PathBuf::from(arguments.next().context("missing service queue path")?);
        let cache_path = PathBuf::from(arguments.next().context("missing service cache path")?);
        let (sender, receiver) = mpsc::channel();
        let stopped = Arc::new(AtomicBool::new(false));
        let signal = Arc::clone(&stopped);
        let status = service_control_handler::register(NAME, move |control| match control {
            ServiceControl::Stop | ServiceControl::Shutdown => {
                signal.store(true, Ordering::Release);
                let _ = sender.send(());
                ServiceControlHandlerResult::NoError
            }
            ServiceControl::Interrogate => ServiceControlHandlerResult::NoError,
            _ => ServiceControlHandlerResult::NotImplemented,
        })?;
        status.set_service_status(ServiceStatus {
            service_type: ServiceType::OWN_PROCESS,
            current_state: ServiceState::Running,
            controls_accepted: ServiceControlAccept::STOP | ServiceControlAccept::SHUTDOWN,
            exit_code: ServiceExitCode::Win32(0),
            checkpoint: 0,
            wait_hint: Duration::default(),
            process_id: None,
        })?;
        let mut queue = Queue::open(&queue_path)?;
        fs::create_dir_all(&cache_path)?;
        while !stopped.load(Ordering::Acquire) {
            if !process_one(&mut queue, &cache_path)? {
                let _ = receiver.recv_timeout(Duration::from_millis(25));
            }
        }
        status.set_service_status(ServiceStatus {
            service_type: ServiceType::OWN_PROCESS,
            current_state: ServiceState::Stopped,
            controls_accepted: ServiceControlAccept::empty(),
            exit_code: ServiceExitCode::Win32(0),
            checkpoint: 0,
            wait_hint: Duration::default(),
            process_id: None,
        })?;
        Ok(())
    }
}

fn main() -> Result<()> {
    let mut args = std::env::args().skip(1);
    let action = args.next().unwrap_or_default();
    #[cfg(windows)]
    if action == "--service" {
        return windows_worker_service::dispatch();
    }
    if action == "--probe" {
        println!(
            "backend={BACKEND} available=yes capabilities=00000001 queue_version={QUEUE_VERSION}"
        );
        return Ok(());
    }
    if action == "--transform" {
        let module = match args.next().as_deref() {
            Some("classic") => false,
            Some("module") => true,
            _ => bail!("script kind must be classic or module"),
        };
        let target = args.next().unwrap_or_else(|| DEFAULT_TARGET.into());
        let mut source = String::new();
        io::stdin()
            .take((MAX_INPUT + 1) as u64)
            .read_to_string(&mut source)?;
        if source.len() > MAX_INPUT {
            bail!("javascript input exceeds limit");
        }
        io::stdout().write_all(&transform(&source, module, &target)?.0)?;
        return Ok(());
    }
    let (queue_path, cache_path) = queue_args(args)?;
    let mut queue = match action.as_str() {
        "--init" | "--init-and-serve" => Queue::create(&queue_path)?,
        "--once" | "--serve" => Queue::open(&queue_path)?,
        _ => bail!(
            "usage: laghu-js-optimize --probe | --transform classic|module [target] | --init|--once|--serve|--init-and-serve QUEUE CACHE"
        ),
    };
    fs::create_dir_all(&cache_path)?;
    if action == "--init" {
        queue.advertise()?;
        return Ok(());
    }
    if action == "--once" {
        let _ = process_one(&mut queue, &cache_path)?;
        return Ok(());
    }
    loop {
        if !process_one(&mut queue, &cache_path)? {
            thread::sleep(Duration::from_millis(25));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn minifies_classic_without_top_level_mangling() {
        let (output, matrix, _) = transform(
            "function publicName(longLocal){ return longLocal + 1; }",
            false,
            "last 2 chrome versions",
        )
        .unwrap();
        let text = String::from_utf8(output).unwrap();
        assert!(text.contains("publicName"));
        assert!(!text.contains("longLocal"));
        assert!(matrix.contains("chrome"));
    }

    #[test]
    fn preserves_modules() {
        let output = transform(
            "export const answer = 40 + 2;",
            true,
            "last 2 chrome versions",
        )
        .unwrap()
        .0;
        assert!(String::from_utf8(output).unwrap().contains("export"));
    }

    #[test]
    fn preserves_directives_legal_comments_and_properties() {
        let output = transform(
            "/*! @license Laghu fixture */ 'use strict'; function publicName(longLocal) { return longLocal.publicProperty; }",
            false,
            "last 2 chrome versions",
        )
        .unwrap()
        .0;
        let text = String::from_utf8(output).unwrap();
        assert!(text.contains("@license Laghu fixture"));
        assert!(text.contains("use strict"));
        assert!(text.contains("publicName"));
        assert!(text.contains("publicProperty"));
        assert!(!text.contains("longLocal"));
    }

    #[test]
    fn rejects_file_and_environment_queries() {
        assert!(resolved_target("$BROWSERSLIST").is_err());
        assert!(resolved_target("extends ../targets").is_err());
    }

    #[test]
    fn certifies_only_safe_classic_concatenation() {
        let flags = transform(
            "console.log('one'); console.log('two');",
            false,
            "last 2 chrome versions",
        )
        .unwrap()
        .2;
        assert_eq!(flags & FLAG_URL_INDEPENDENT, FLAG_URL_INDEPENDENT);
        assert_eq!(flags & FLAG_CONCAT_SAFE, FLAG_CONCAT_SAFE);
        assert_eq!(
            flags & FLAG_TOP_LEVEL_DECLARATION_FREE,
            FLAG_TOP_LEVEL_DECLARATION_FREE
        );
        let declaration = transform(
            "let topLevel = 1; console.log(topLevel);",
            false,
            "last 2 chrome versions",
        )
        .unwrap()
        .2;
        assert_eq!(declaration & FLAG_CONCAT_SAFE, 0);
        assert_eq!(declaration & FLAG_TOP_LEVEL_DECLARATION_FREE, 0);
    }

    #[test]
    fn rejects_url_sensitive_programs() {
        let current = transform(
            "console.log(document.currentScript); console.log('current script');",
            false,
            "last 2 chrome versions",
        )
        .unwrap()
        .2;
        assert_eq!(current & FLAG_URL_INDEPENDENT, 0);
        let relative = transform(
            "import value from './value.js'; console.log(value); export { value }",
            true,
            "last 2 chrome versions",
        )
        .unwrap()
        .2;
        assert_eq!(relative & FLAG_MODULE, FLAG_MODULE);
        assert_eq!(relative & FLAG_URL_INDEPENDENT, 0);
        let meta = transform(
            "console.log(import.meta.url); console.log('module url');",
            true,
            "last 2 chrome versions",
        )
        .unwrap()
        .2;
        assert_eq!(meta & FLAG_URL_INDEPENDENT, 0);
    }

    #[test]
    fn consumes_queue_job_and_publishes_checksums() {
        let root = std::env::temp_dir().join(format!(
            "laghu-js-test-{}-{}",
            std::process::id(),
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        let queue_path = root.join("javascript.queue");
        let cache = root.join("cache");
        fs::create_dir_all(&root).unwrap();
        let mut queue = Queue::create(&queue_path).unwrap();
        let source = b"function publicName(longLocal){ return longLocal + 1; }";
        let source_hash = sha256(source);
        let index_key = sha256(b"test-index");
        let base = HEADER_SIZE;
        write_u32(&mut queue.map, base, SLOT_READY);
        write_u32(&mut queue.map, base + 4, JOB_JAVASCRIPT);
        write_u64(&mut queue.map, base + 8, source.len() as u64);
        write_fixed(&mut queue.map[base + 16..base + 81], &index_key).unwrap();
        write_fixed(&mut queue.map[base + 81..base + 1105], "/app.js").unwrap();
        write_fixed(&mut queue.map[base + 1105..base + 1361], &source_hash).unwrap();
        write_fixed(&mut queue.map[base + 1425..base + 1490], &sha256(b"policy")).unwrap();
        write_fixed(
            &mut queue.map[base + 1619..base + 2131],
            "last 2 chrome versions",
        )
        .unwrap();
        queue.map[base + SLOT_SIZE..base + SLOT_SIZE + source.len()].copy_from_slice(source);
        queue.map.flush().unwrap();
        assert!(process_one(&mut queue, &cache).unwrap());
        assert!(cache.join(format!("index-{index_key}.meta")).is_file());
        assert!(fs::read_dir(&cache).unwrap().any(|entry| {
            entry
                .unwrap()
                .file_name()
                .to_string_lossy()
                .starts_with("javascript-")
        }));
        drop(queue);
        fs::remove_dir_all(root).unwrap();
    }
}
