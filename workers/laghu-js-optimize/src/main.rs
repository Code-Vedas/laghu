// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

use std::{
    fs::{self, File, OpenOptions},
    io::{self, Read, Write},
    path::{Path, PathBuf},
    sync::atomic::{AtomicBool, Ordering},
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
        FileName, GLOBALS, Globals, Mark, SourceMap, comments::SingleThreadedComments,
        source_map::DefaultSourceMapGenConfig, sync::Lrc,
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

#[allow(dead_code)]
mod persisted_state {
    include!(concat!(env!("OUT_DIR"), "/persisted_state.rs"));
}

use persisted_state::*;

const MAX_INPUT: usize = 2 * 1024 * 1024;
const DEFAULT_TARGET: &str = "defaults and supports es6-module and not dead";
const JOB_JAVASCRIPT: u32 = 3;
const BACKEND: &str = "laghu-swc-75.0.0-js-v3";
const CATALOG_MAGIC: u64 = 0x4c41_4748_554a_5343;
const CATALOG_SIZE: usize = 1_908;
const FLAG_MODULE: u32 = 1;
const FLAG_URL_INDEPENDENT: u32 = 2;
const FLAG_CONCAT_SAFE: u32 = 4;
const FLAG_TOP_LEVEL_DECLARATION_FREE: u32 = 8;
const FLAG_DEFER_SAFE: u32 = 16;
const FILTER_MODULE: u64 = 1;
const OPERATIONAL_STALE_SECONDS: u64 = 45;
const OPERATIONAL_FAILURE_CACHE: usize = 1;
const OPERATIONAL_FAILURE_QUEUE: usize = 2;
const OPERATIONAL_FAILURE_TRANSFORM: usize = 4;
const OPERATIONAL_LATENCY_LIMITS: [u64; 11] = [
    1_000, 5_000, 10_000, 25_000, 50_000, 100_000, 250_000, 500_000, 1_000_000, 2_500_000,
    5_000_000,
];
static OPERATIONAL_DIAGNOSTIC_EMITTED: AtomicBool = AtomicBool::new(false);

enum Operational {
    Active(OperationalRegistry),
    Unavailable,
}

struct OperationalRegistry {
    file: File,
    map: MmapMut,
    slot: usize,
    generation: u64,
}

impl OperationalRegistry {
    fn open(cache: &Path) -> Result<Self> {
        fs::create_dir_all(cache)?;
        let path = operational_path(cache);
        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .open(path)?;
        file.try_lock_exclusive()?;
        let size = usize::try_from(OPERATIONAL_HEADER_SIZE)?
            .checked_add(
                usize::try_from(OPERATIONAL_SLOT_COUNT)?
                    .checked_mul(usize::try_from(OPERATIONAL_SLOT_SIZE)?)
                    .context("operational registry size overflow")?,
            )
            .context("operational registry size overflow")?;
        if file.metadata()?.len() == 0 {
            file.set_len(size as u64)?;
        }
        if file.metadata()?.len() != size as u64 {
            FileExt::unlock(&file)?;
            bail!("incompatible operational registry size");
        }
        let mut map = unsafe { MmapOptions::new().map_mut(&file)? };
        if read_u64(&map, OPERATIONAL_HEADER_MAGIC_OFFSET as usize) == 0 {
            map.fill(0);
            write_u64(
                &mut map,
                OPERATIONAL_HEADER_MAGIC_OFFSET as usize,
                OPERATIONAL_MAGIC,
            );
            write_u32(
                &mut map,
                OPERATIONAL_HEADER_VERSION_OFFSET as usize,
                OPERATIONAL_VERSION,
            );
            write_u32(
                &mut map,
                OPERATIONAL_HEADER_SLOT_COUNT_OFFSET as usize,
                OPERATIONAL_SLOT_COUNT,
            );
            write_u64(
                &mut map,
                OPERATIONAL_HEADER_GENERATION_OFFSET as usize,
                epoch_seconds(),
            );
        }
        if read_u64(&map, OPERATIONAL_HEADER_MAGIC_OFFSET as usize) != OPERATIONAL_MAGIC
            || read_u32(&map, OPERATIONAL_HEADER_VERSION_OFFSET as usize) != OPERATIONAL_VERSION
            || read_u32(&map, OPERATIONAL_HEADER_SLOT_COUNT_OFFSET as usize)
                != OPERATIONAL_SLOT_COUNT
        {
            FileExt::unlock(&file)?;
            bail!("incompatible operational registry");
        }
        let now = epoch_seconds();
        let slot = (0..OPERATIONAL_SLOT_COUNT as usize)
            .find(|slot| {
                let base = operational_slot_base(*slot);
                let sequence = read_u64(&map, base + OPERATIONAL_SLOT_SEQUENCE_OFFSET as usize);
                let active = read_u32(&map, base + OPERATIONAL_SLOT_ACTIVE_OFFSET as usize);
                let heartbeat = read_u64(&map, base + OPERATIONAL_SLOT_HEARTBEAT_OFFSET as usize);
                active == 0
                    || (sequence & 1 == 0
                        && (heartbeat == 0
                            || heartbeat > now
                            || now.saturating_sub(heartbeat) > OPERATIONAL_STALE_SECONDS))
            })
            .context("operational registry has no free process slot")?;
        let generation =
            read_u64(&map, OPERATIONAL_HEADER_GENERATION_OFFSET as usize).saturating_add(1);
        write_u64(
            &mut map,
            OPERATIONAL_HEADER_GENERATION_OFFSET as usize,
            generation,
        );
        let base = operational_slot_base(slot);
        map[base..base + OPERATIONAL_SLOT_SIZE as usize].fill(0);
        write_u64(
            &mut map,
            base + OPERATIONAL_SLOT_SEQUENCE_OFFSET as usize,
            1,
        );
        write_u64(
            &mut map,
            base + OPERATIONAL_SLOT_GENERATION_OFFSET as usize,
            generation,
        );
        write_u64(
            &mut map,
            base + OPERATIONAL_SLOT_HEARTBEAT_OFFSET as usize,
            now,
        );
        write_u32(&mut map, base + OPERATIONAL_SLOT_SURFACE_OFFSET as usize, 3); // worker surface
        write_u32(
            &mut map,
            base + OPERATIONAL_SLOT_PROCESS_KIND_OFFSET as usize,
            2,
        ); // javascript process
        map[base + OPERATIONAL_SLOT_REQUIRED_OFFSET as usize] = 1;
        map[base + OPERATIONAL_SLOT_HEALTHY_OFFSET as usize] = 1;
        write_u32(&mut map, base + OPERATIONAL_SLOT_ACTIVE_OFFSET as usize, 1);
        write_u64(
            &mut map,
            base + OPERATIONAL_SLOT_SEQUENCE_OFFSET as usize,
            2,
        );
        map.flush_async()?;
        FileExt::unlock(&file)?;
        Ok(Self {
            file,
            map,
            slot,
            generation,
        })
    }

    fn update(&mut self, update: impl FnOnce(&mut [u8])) {
        if self.file.try_lock_exclusive().is_err() {
            return;
        }
        let base = operational_slot_base(self.slot);
        let valid = read_u64(
            &self.map,
            base + OPERATIONAL_SLOT_GENERATION_OFFSET as usize,
        ) == self.generation
            && read_u32(&self.map, base + OPERATIONAL_SLOT_ACTIVE_OFFSET as usize) != 0;
        let sequence = read_u64(&self.map, base + OPERATIONAL_SLOT_SEQUENCE_OFFSET as usize);
        if valid && sequence & 1 == 0 {
            write_u64(
                &mut self.map,
                base + OPERATIONAL_SLOT_SEQUENCE_OFFSET as usize,
                sequence.saturating_add(1),
            );
            update(&mut self.map[base..base + OPERATIONAL_SLOT_SIZE as usize]);
            write_u64(
                &mut self.map,
                base + OPERATIONAL_SLOT_SEQUENCE_OFFSET as usize,
                sequence.saturating_add(2),
            );
            let _ = self.map.flush_async();
        }
        let _ = FileExt::unlock(&self.file);
    }

    fn heartbeat(&mut self, queue: &Queue) {
        let (capacity, occupied) = queue.snapshot();
        self.update(|slot| {
            slot[OPERATIONAL_SLOT_HEALTHY_OFFSET as usize] = 1;
            write_u64(
                slot,
                OPERATIONAL_SLOT_QUEUE_CAPACITY_OFFSET as usize,
                capacity,
            );
            write_u64(
                slot,
                OPERATIONAL_SLOT_QUEUE_OCCUPIED_OFFSET as usize,
                occupied.min(capacity),
            );
            write_u64(
                slot,
                OPERATIONAL_SLOT_HEARTBEAT_OFFSET as usize,
                epoch_seconds(),
            );
        });
    }

    fn record(&mut self, elapsed_microseconds: u64, failure: Option<usize>) {
        self.update(|slot| {
            let bucket = OPERATIONAL_LATENCY_LIMITS
                .iter()
                .position(|limit| elapsed_microseconds <= *limit)
                .unwrap_or(OPERATIONAL_LATENCY_LIMITS.len());
            saturating_add(
                slot,
                OPERATIONAL_SLOT_LATENCY_BUCKETS_OFFSET as usize + bucket * 8,
                1,
            );
            saturating_add(slot, OPERATIONAL_SLOT_LATENCY_COUNT_OFFSET as usize, 1);
            saturating_add(
                slot,
                OPERATIONAL_SLOT_LATENCY_SUM_OFFSET as usize,
                elapsed_microseconds,
            );
            if let Some(failure) = failure.filter(|failure| *failure < 6) {
                saturating_add(
                    slot,
                    OPERATIONAL_SLOT_FAILURES_OFFSET as usize + failure * 8,
                    1,
                );
            }
        });
    }

    fn release(&mut self) {
        if self.file.try_lock_exclusive().is_err() {
            return;
        }
        let base = operational_slot_base(self.slot);
        let sequence = read_u64(&self.map, base + OPERATIONAL_SLOT_SEQUENCE_OFFSET as usize);
        if sequence & 1 == 0
            && read_u64(
                &self.map,
                base + OPERATIONAL_SLOT_GENERATION_OFFSET as usize,
            ) == self.generation
            && read_u32(&self.map, base + OPERATIONAL_SLOT_ACTIVE_OFFSET as usize) != 0
        {
            write_u64(
                &mut self.map,
                base + OPERATIONAL_SLOT_SEQUENCE_OFFSET as usize,
                sequence.saturating_add(1),
            );
            self.map[base + OPERATIONAL_SLOT_HEALTHY_OFFSET as usize] = 0;
            write_u32(
                &mut self.map,
                base + OPERATIONAL_SLOT_ACTIVE_OFFSET as usize,
                0,
            );
            write_u64(
                &mut self.map,
                base + OPERATIONAL_SLOT_SEQUENCE_OFFSET as usize,
                sequence.saturating_add(2),
            );
            let _ = self.map.flush();
        }
        let _ = FileExt::unlock(&self.file);
    }
}

impl Drop for OperationalRegistry {
    fn drop(&mut self) {
        self.release();
    }
}

impl Operational {
    fn open(cache: &Path) -> Self {
        match OperationalRegistry::open(cache) {
            Ok(registry) => Self::Active(registry),
            Err(_) => {
                if !OPERATIONAL_DIAGNOSTIC_EMITTED.swap(true, Ordering::Relaxed) {
                    eprintln!("laghu-js-optimize: operational registry unavailable");
                }
                Self::Unavailable
            }
        }
    }

    fn heartbeat(&mut self, queue: &Queue) {
        if let Self::Active(registry) = self {
            registry.heartbeat(queue);
        }
    }

    fn record(&mut self, elapsed_microseconds: u64, failure: Option<usize>) {
        if let Self::Active(registry) = self {
            registry.record(elapsed_microseconds, failure);
        }
    }
}

fn operational_slot_base(slot: usize) -> usize {
    OPERATIONAL_HEADER_SIZE as usize + slot * OPERATIONAL_SLOT_SIZE as usize
}

fn operational_path(cache: &Path) -> PathBuf {
    let mut path = cache.as_os_str().to_os_string();
    path.push(".laghu-operations-v4");
    PathBuf::from(path)
}

fn saturating_add(bytes: &mut [u8], offset: usize, increment: u64) {
    write_u64(
        bytes,
        offset,
        read_u64(bytes, offset).saturating_add(increment),
    );
}

fn epoch_seconds() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |time| time.as_secs())
}
const FILTER_SOURCE_MAP: u64 = 2;

#[derive(Default)]
struct Eligibility {
    url_independent: bool,
    concat_safe: bool,
    defer_safe: bool,
}

impl Visit for Eligibility {
    fn visit_ident(&mut self, node: &Ident) {
        if matches!(&*node.sym, "eval" | "Function" | "currentScript") {
            self.url_independent = false;
            self.concat_safe = false;
            self.defer_safe = false;
        }
        if matches!(&*node.sym, "document" | "setTimeout" | "setInterval") {
            self.defer_safe = false;
        }
    }

    fn visit_member_expr(&mut self, node: &MemberExpr) {
        if node.prop.is_ident_with("currentScript") {
            self.url_independent = false;
            self.concat_safe = false;
            self.defer_safe = false;
        }
        if node.obj.is_ident_ref_to("document")
            && matches!(
                node.prop.as_ident().map(|ident| &*ident.sym),
                Some("write" | "writeln" | "open" | "close" | "currentScript")
            )
        {
            self.defer_safe = false;
        }
        node.visit_children_with(self);
    }

    fn visit_call_expr(&mut self, node: &swc_core::ecma::ast::CallExpr) {
        if matches!(node.callee, Callee::Import(_)) {
            self.url_independent = false;
            self.concat_safe = false;
            self.defer_safe = false;
        }
        node.visit_children_with(self);
    }

    fn visit_meta_prop_expr(&mut self, node: &MetaPropExpr) {
        if node.kind == MetaPropKind::ImportMeta {
            self.url_independent = false;
            self.concat_safe = false;
            self.defer_safe = false;
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
        defer_safe: !module,
    };
    program.visit_with(&mut result);
    (u32::from(module) * FLAG_MODULE)
        | (u32::from(result.url_independent) * FLAG_URL_INDEPENDENT)
        | (u32::from(result.concat_safe) * FLAG_CONCAT_SAFE)
        | (u32::from(top_level_declaration_free) * FLAG_TOP_LEVEL_DECLARATION_FREE)
        | (u32::from(result.defer_safe) * FLAG_DEFER_SAFE)
}

fn c_string(bytes: &[u8]) -> Result<&str> {
    let end = bytes
        .iter()
        .position(|byte| *byte == 0)
        .context("unterminated queue string")?;
    std::str::from_utf8(&bytes[..end]).context("queue string is not UTF-8")
}

fn read_u32(bytes: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(
        bytes[offset..offset + 4]
            .try_into()
            .expect("fixed queue layout"),
    )
}

fn read_u64(bytes: &[u8], offset: usize) -> u64 {
    u64::from_le_bytes(
        bytes[offset..offset + 8]
            .try_into()
            .expect("fixed queue layout"),
    )
}

fn write_u32(bytes: &mut [u8], offset: usize, value: u32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

fn write_u64(bytes: &mut [u8], offset: usize, value: u64) {
    bytes[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
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

struct TransformResult {
    output: Vec<u8>,
    source_map: Vec<u8>,
    matrix: String,
    flags: u32,
}

fn transform_artifact(
    source: &str,
    source_name: &str,
    module: bool,
    target: &str,
    emit_source_map: bool,
) -> Result<TransformResult> {
    let matrix = resolved_target(target)?;
    GLOBALS.set(&Globals::new(), || {
        let cm: Lrc<SourceMap> = Default::default();
        let comments = SingleThreadedComments::default();
        let file = cm.new_source_file(
            FileName::Custom(source_name.into()).into(),
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
        let mut mappings = Vec::new();
        {
            let writer = JsWriter::new(
                cm.clone(),
                "\n",
                &mut output,
                emit_source_map.then_some(&mut mappings),
            );
            let mut emitter = Emitter {
                cfg: CodegenConfig::default().with_minify(true),
                comments: Some(&comments),
                cm: cm.clone(),
                wr: Box::new(writer),
            };
            emitter.emit_program(&program)?;
        }
        if output.len() >= source.len() || output.len() > MAX_INPUT {
            bail!("javascript output is not smaller");
        }
        let mut source_map = Vec::new();
        if emit_source_map {
            cm.build_source_map(&mappings, None, DefaultSourceMapGenConfig)
                .to_writer(&mut source_map)?;
            if source_map.len() > MAX_INPUT
                || source_map
                    .windows(b"\"sourcesContent\"".len())
                    .any(|value| value == b"\"sourcesContent\"")
            {
                bail!("invalid generated source map");
            }
        }
        Ok(TransformResult {
            output,
            source_map,
            matrix,
            flags,
        })
    })
}

fn transform(source: &str, module: bool, target: &str) -> Result<(Vec<u8>, String, u32)> {
    let result = transform_artifact(source, "input.js", module, target, false)?;
    Ok((result.output, result.matrix, result.flags))
}

struct Queue {
    file: File,
    map: MmapMut,
    slots: usize,
    payload_size: usize,
    last_advertised: Option<u64>,
}

struct Job {
    index_key: String,
    request_path: String,
    validator: String,
    policy_key: String,
    target: String,
    module: bool,
    source_map: bool,
    source: Vec<u8>,
}

impl Queue {
    fn create(path: &Path) -> Result<Self> {
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent)?;
        }
        let slots = 8usize;
        let length =
            QUEUE_HEADER_SIZE as usize + slots * (QUEUE_SLOT_HEADER_SIZE as usize + MAX_INPUT);
        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(true)
            .open(path)?;
        file.set_len(length as u64)?;
        let mut map = unsafe { MmapOptions::new().map_mut(&file)? };
        map.fill(0);
        write_u64(&mut map, QUEUE_HEADER_MAGIC_OFFSET as usize, QUEUE_MAGIC);
        write_u32(
            &mut map,
            QUEUE_HEADER_VERSION_OFFSET as usize,
            QUEUE_VERSION,
        );
        write_u32(
            &mut map,
            QUEUE_HEADER_SLOT_COUNT_OFFSET as usize,
            slots as u32,
        );
        write_u64(
            &mut map,
            QUEUE_HEADER_SLOT_PAYLOAD_SIZE_OFFSET as usize,
            MAX_INPUT as u64,
        );
        map.flush()?;
        Ok(Self {
            file,
            map,
            slots,
            payload_size: MAX_INPUT,
            last_advertised: None,
        })
    }

    fn open(path: &Path) -> Result<Self> {
        let file = OpenOptions::new().read(true).write(true).open(path)?;
        let map = unsafe { MmapOptions::new().map_mut(&file)? };
        if map.len() < QUEUE_HEADER_SIZE as usize
            || read_u64(&map, QUEUE_HEADER_MAGIC_OFFSET as usize) != QUEUE_MAGIC
            || read_u32(&map, QUEUE_HEADER_VERSION_OFFSET as usize) != QUEUE_VERSION
        {
            bail!("incompatible javascript queue");
        }
        let slots = read_u32(&map, QUEUE_HEADER_SLOT_COUNT_OFFSET as usize) as usize;
        let payload_size = usize::try_from(read_u64(
            &map,
            QUEUE_HEADER_SLOT_PAYLOAD_SIZE_OFFSET as usize,
        ))?;
        let expected = (QUEUE_HEADER_SIZE as usize)
            .checked_add(
                slots
                    .checked_mul(
                        (QUEUE_SLOT_HEADER_SIZE as usize)
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
            last_advertised: None,
        })
    }

    fn advertise(&mut self) -> Result<()> {
        let now = SystemTime::now().duration_since(UNIX_EPOCH)?.as_secs();
        if self.last_advertised == Some(now) {
            return Ok(());
        }
        if self.file.try_lock_exclusive().is_err() {
            return Ok(());
        }
        let result: Result<()> = (|| {
            write_u32(&mut self.map, QUEUE_HEADER_CAPABILITIES_OFFSET as usize, 1);
            write_u64(&mut self.map, QUEUE_HEADER_HEARTBEAT_OFFSET as usize, now);
            self.map[QUEUE_HEADER_BACKEND_ID_OFFSET as usize..QUEUE_HEADER_SIZE as usize].fill(0);
            self.map[QUEUE_HEADER_BACKEND_ID_OFFSET as usize
                ..QUEUE_HEADER_BACKEND_ID_OFFSET as usize + BACKEND.len()]
                .copy_from_slice(BACKEND.as_bytes());
            Ok(())
        })();
        let unlock = FileExt::unlock(&self.file);
        if let Err(error) = result {
            let _ = unlock;
            return Err(error);
        }
        unlock?;
        self.last_advertised = Some(now);
        Ok(())
    }

    fn take(&mut self) -> Result<Option<Job>> {
        if self.file.try_lock_exclusive().is_err() {
            return Ok(None);
        }
        let result = (|| {
            let read =
                read_u32(&self.map, QUEUE_HEADER_NEXT_READ_OFFSET as usize) as usize % self.slots;
            let base = queue_slot_base(read, self.payload_size);
            if read_u32(&self.map, base + QUEUE_SLOT_STATE_OFFSET as usize) != QUEUE_SLOT_READY {
                return Ok(None);
            }
            let length = usize::try_from(read_u64(
                &self.map,
                base + QUEUE_SLOT_PAYLOAD_LENGTH_OFFSET as usize,
            ))?;
            let valid = read_u32(&self.map, base + QUEUE_SLOT_KIND_OFFSET as usize)
                == JOB_JAVASCRIPT
                && length > 0
                && length <= MAX_INPUT
                && length <= self.payload_size;
            let job = if valid {
                Some(Job {
                    index_key: c_string(
                        &self.map[base + QUEUE_SLOT_INDEX_KEY_OFFSET as usize
                            ..base + QUEUE_SLOT_REQUEST_PATH_OFFSET as usize],
                    )?
                    .to_owned(),
                    request_path: c_string(
                        &self.map[base + QUEUE_SLOT_REQUEST_PATH_OFFSET as usize
                            ..base + QUEUE_SLOT_VALIDATOR_OFFSET as usize],
                    )?
                    .to_owned(),
                    validator: c_string(
                        &self.map[base + QUEUE_SLOT_VALIDATOR_OFFSET as usize
                            ..base + QUEUE_SLOT_CONTENT_TYPE_OFFSET as usize],
                    )?
                    .to_owned(),
                    policy_key: c_string(
                        &self.map[base + QUEUE_SLOT_POLICY_KEY_OFFSET as usize
                            ..base + QUEUE_SLOT_PROVIDER_ID_OFFSET as usize],
                    )?
                    .to_owned(),
                    target: c_string(
                        &self.map[base + QUEUE_SLOT_JAVASCRIPT_TARGET_OFFSET as usize
                            ..base + QUEUE_SLOT_FILTERS_OFFSET as usize],
                    )?
                    .to_owned(),
                    module: read_u64(&self.map, base + QUEUE_SLOT_FILTERS_OFFSET as usize)
                        & FILTER_MODULE
                        != 0,
                    source_map: read_u64(&self.map, base + QUEUE_SLOT_FILTERS_OFFSET as usize)
                        & FILTER_SOURCE_MAP
                        != 0,
                    source: self.map[base + QUEUE_SLOT_HEADER_SIZE as usize
                        ..base + QUEUE_SLOT_HEADER_SIZE as usize + length]
                        .to_vec(),
                })
            } else {
                None
            };
            self.map[base..base + QUEUE_SLOT_HEADER_SIZE as usize + length.min(self.payload_size)]
                .fill(0);
            write_u32(
                &mut self.map,
                QUEUE_HEADER_NEXT_READ_OFFSET as usize,
                ((read + 1) % self.slots) as u32,
            );
            Ok(job)
        })();
        let unlock = FileExt::unlock(&self.file);
        if let Err(error) = result {
            let _ = unlock;
            return Err(error);
        }
        unlock?;
        result
    }

    fn snapshot(&self) -> (u64, u64) {
        let occupied = (0..self.slots)
            .filter(|slot| {
                read_u32(
                    &self.map,
                    queue_slot_base(*slot, self.payload_size) + QUEUE_SLOT_STATE_OFFSET as usize,
                ) == QUEUE_SLOT_READY
            })
            .count();
        (self.slots as u64, occupied as u64)
    }
}

fn queue_slot_base(slot: usize, payload_size: usize) -> usize {
    QUEUE_HEADER_SIZE as usize + slot * (QUEUE_SLOT_HEADER_SIZE as usize + payload_size)
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

fn publish(cache: &Path, job: &Job, output: &[u8], source_map: &[u8]) -> Result<Vec<u8>> {
    if job.index_key.len() != 64 || job.validator.len() != 64 {
        bail!("invalid javascript cache key");
    }
    fs::create_dir_all(cache)?;
    let mut published = output.to_vec();
    if !source_map.is_empty() {
        let map_variant = sha256(source_map);
        let directive = format!("\n//# sourceMappingURL=/.laghu/js/{map_variant}.map");
        if published.len() + directive.len() >= job.source.len()
            || published.len() + directive.len() > MAX_INPUT
        {
            bail!("javascript with source map reference is not smaller");
        }
        atomic_write(
            &cache.join(format!("variant-{map_variant}.bin")),
            source_map,
        )?;
        let mut map_metadata = vec![0u8; CACHE_ARTIFACT_SIZE as usize];
        write_u64(
            &mut map_metadata,
            CACHE_ARTIFACT_MAGIC_OFFSET as usize,
            CACHE_ARTIFACT_MAGIC,
        );
        write_u32(
            &mut map_metadata,
            CACHE_ARTIFACT_VERSION_OFFSET as usize,
            CACHE_ARTIFACT_VERSION,
        );
        write_u64(
            &mut map_metadata,
            CACHE_ARTIFACT_LENGTH_OFFSET as usize,
            source_map.len() as u64,
        );
        write_fixed(
            &mut map_metadata[CACHE_ARTIFACT_VARIANT_KEY_OFFSET as usize
                ..CACHE_ARTIFACT_PAYLOAD_HASH_OFFSET as usize],
            &map_variant,
        )?;
        write_fixed(
            &mut map_metadata[CACHE_ARTIFACT_PAYLOAD_HASH_OFFSET as usize
                ..CACHE_ARTIFACT_VALIDATOR_OFFSET as usize],
            &map_variant,
        )?;
        write_fixed(
            &mut map_metadata[CACHE_ARTIFACT_VALIDATOR_OFFSET as usize
                ..CACHE_ARTIFACT_CONTENT_TYPE_OFFSET as usize],
            &map_variant,
        )?;
        write_fixed(
            &mut map_metadata[CACHE_ARTIFACT_CONTENT_TYPE_OFFSET as usize
                ..CACHE_ARTIFACT_BACKEND_ID_OFFSET as usize],
            "application/json",
        )?;
        write_fixed(
            &mut map_metadata[CACHE_ARTIFACT_BACKEND_ID_OFFSET as usize
                ..CACHE_ARTIFACT_BACKEND_ID_END_OFFSET as usize],
            BACKEND,
        )?;
        atomic_write(
            &cache.join(format!("index-{map_variant}.meta")),
            &map_metadata,
        )?;
        published.extend_from_slice(directive.as_bytes());
    }
    let variant = sha256(&published);
    atomic_write(&cache.join(format!("variant-{variant}.bin")), &published)?;
    let mut metadata = vec![0u8; CACHE_ARTIFACT_SIZE as usize];
    write_u64(
        &mut metadata,
        CACHE_ARTIFACT_MAGIC_OFFSET as usize,
        CACHE_ARTIFACT_MAGIC,
    );
    write_u32(
        &mut metadata,
        CACHE_ARTIFACT_VERSION_OFFSET as usize,
        CACHE_ARTIFACT_VERSION,
    );
    write_u64(
        &mut metadata,
        CACHE_ARTIFACT_LENGTH_OFFSET as usize,
        published.len() as u64,
    );
    write_fixed(
        &mut metadata[CACHE_ARTIFACT_VARIANT_KEY_OFFSET as usize
            ..CACHE_ARTIFACT_PAYLOAD_HASH_OFFSET as usize],
        &variant,
    )?;
    write_fixed(
        &mut metadata
            [CACHE_ARTIFACT_PAYLOAD_HASH_OFFSET as usize..CACHE_ARTIFACT_VALIDATOR_OFFSET as usize],
        &variant,
    )?;
    write_fixed(
        &mut metadata
            [CACHE_ARTIFACT_VALIDATOR_OFFSET as usize..CACHE_ARTIFACT_CONTENT_TYPE_OFFSET as usize],
        &job.validator,
    )?;
    write_fixed(
        &mut metadata[CACHE_ARTIFACT_CONTENT_TYPE_OFFSET as usize
            ..CACHE_ARTIFACT_BACKEND_ID_OFFSET as usize],
        "application/javascript",
    )?;
    write_fixed(
        &mut metadata[CACHE_ARTIFACT_BACKEND_ID_OFFSET as usize
            ..CACHE_ARTIFACT_BACKEND_ID_END_OFFSET as usize],
        BACKEND,
    )?;
    atomic_write(
        &cache.join(format!("index-{}.meta", job.index_key)),
        &metadata,
    )?;
    atomic_write(&cache.join(format!("index-{variant}.meta")), &metadata)?;
    Ok(published)
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
    write_u32(&mut catalog, 8, 3);
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

enum ProcessOutcome {
    Idle,
    Success,
    Failure(usize),
}

fn process_one(queue: &mut Queue, cache: &Path) -> ProcessOutcome {
    if queue.advertise().is_err() {
        return ProcessOutcome::Failure(OPERATIONAL_FAILURE_QUEUE);
    }
    let job = match queue.take() {
        Ok(Some(job)) => job,
        Ok(None) => return ProcessOutcome::Idle,
        Err(_) => return ProcessOutcome::Failure(OPERATIONAL_FAILURE_QUEUE),
    };
    let source = match std::str::from_utf8(&job.source) {
        Ok(source) => source,
        Err(_) => return ProcessOutcome::Failure(OPERATIONAL_FAILURE_TRANSFORM),
    };
    let result = match transform_artifact(
        source,
        &format!("laghu-{}.js", &job.validator[..16.min(job.validator.len())]),
        job.module,
        &job.target,
        job.source_map,
    ) {
        Ok(result) => result,
        Err(_) => return ProcessOutcome::Failure(OPERATIONAL_FAILURE_TRANSFORM),
    };
    let published = match publish(cache, &job, &result.output, &result.source_map) {
        Ok(published) => published,
        Err(_) if !result.source_map.is_empty() => {
            match publish(cache, &job, &result.output, &[]) {
                Ok(published) => published,
                Err(_) => return ProcessOutcome::Failure(OPERATIONAL_FAILURE_CACHE),
            }
        }
        Err(_) => return ProcessOutcome::Failure(OPERATIONAL_FAILURE_CACHE),
    };
    if publish_catalog(cache, &job, &published, &result.matrix, result.flags).is_err() {
        return ProcessOutcome::Failure(OPERATIONAL_FAILURE_CACHE);
    }
    ProcessOutcome::Success
}

fn record_process(operational: &mut Operational, outcome: &ProcessOutcome, elapsed: u64) {
    match outcome {
        ProcessOutcome::Success => operational.record(elapsed, None),
        ProcessOutcome::Failure(failure) => operational.record(elapsed, Some(*failure)),
        ProcessOutcome::Idle => {}
    }
}

fn queue_args(mut args: impl Iterator<Item = String>) -> Result<(PathBuf, PathBuf)> {
    let queue = PathBuf::from(args.next().context("missing queue path")?);
    let cache = PathBuf::from(args.next().context("missing cache path")?);
    if args.next().is_some() {
        bail!("unexpected worker argument");
    }
    Ok((queue, cache))
}

fn main() -> Result<()> {
    let mut args = std::env::args().skip(1);
    let action = args.next().unwrap_or_default();
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
        let mut operational = Operational::open(&cache_path);
        operational.heartbeat(&queue);
        let started = SystemTime::now();
        let processed = process_one(&mut queue, &cache_path);
        let elapsed = started.elapsed().map_or(0, |time| time.as_micros() as u64);
        record_process(&mut operational, &processed, elapsed);
        return Ok(());
    }
    let mut operational = Operational::open(&cache_path);
    loop {
        operational.heartbeat(&queue);
        let started = SystemTime::now();
        let processed = process_one(&mut queue, &cache_path);
        let elapsed = started.elapsed().map_or(0, |time| time.as_micros() as u64);
        record_process(&mut operational, &processed, elapsed);
        if !matches!(processed, ProcessOutcome::Success) {
            thread::sleep(Duration::from_millis(25));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn cross_language_root(name: &str) -> PathBuf {
        std::env::temp_dir().join(format!(
            "laghu-js-{name}-{}-{}",
            std::process::id(),
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ))
    }

    fn cross_language_wait(path: &Path) {
        for _ in 0..1_000 {
            if path.is_file() {
                return;
            }
            thread::sleep(Duration::from_millis(10));
        }
        panic!("timed out waiting for {}", path.display());
    }

    fn cross_language_signal(base: &Path, name: &str) -> PathBuf {
        PathBuf::from(format!("{}.{}", base.display(), name))
    }

    fn publish_cross_language_job(queue: &mut Queue) {
        let source = b"console.log('rust producer');";
        let base = queue_slot_base(0, queue.payload_size);
        write_u32(
            &mut queue.map,
            base + QUEUE_SLOT_KIND_OFFSET as usize,
            JOB_JAVASCRIPT,
        );
        write_u64(
            &mut queue.map,
            base + QUEUE_SLOT_PAYLOAD_LENGTH_OFFSET as usize,
            source.len() as u64,
        );
        write_fixed(
            &mut queue.map[base + QUEUE_SLOT_INDEX_KEY_OFFSET as usize
                ..base + QUEUE_SLOT_REQUEST_PATH_OFFSET as usize],
            &sha256(b"rust-cross-language-index"),
        )
        .unwrap();
        write_fixed(
            &mut queue.map[base + QUEUE_SLOT_REQUEST_PATH_OFFSET as usize
                ..base + QUEUE_SLOT_VALIDATOR_OFFSET as usize],
            "/cross-language.js",
        )
        .unwrap();
        write_fixed(
            &mut queue.map[base + QUEUE_SLOT_VALIDATOR_OFFSET as usize
                ..base + QUEUE_SLOT_CONTENT_TYPE_OFFSET as usize],
            &sha256(b"rust-cross-language-validator"),
        )
        .unwrap();
        write_fixed(
            &mut queue.map[base + QUEUE_SLOT_CONTENT_TYPE_OFFSET as usize
                ..base + QUEUE_SLOT_POLICY_KEY_OFFSET as usize],
            "application/javascript",
        )
        .unwrap();
        write_fixed(
            &mut queue.map[base + QUEUE_SLOT_POLICY_KEY_OFFSET as usize
                ..base + QUEUE_SLOT_PROVIDER_ID_OFFSET as usize],
            &sha256(b"rust-cross-language-policy"),
        )
        .unwrap();
        write_fixed(
            &mut queue.map[base + QUEUE_SLOT_JAVASCRIPT_TARGET_OFFSET as usize
                ..base + QUEUE_SLOT_FILTERS_OFFSET as usize],
            "last 2 chrome versions",
        )
        .unwrap();
        queue.map[base + QUEUE_SLOT_HEADER_SIZE as usize
            ..base + QUEUE_SLOT_HEADER_SIZE as usize + source.len()]
            .copy_from_slice(source);
        write_u32(
            &mut queue.map,
            base + QUEUE_SLOT_STATE_OFFSET as usize,
            QUEUE_SLOT_READY,
        );
        queue.map.flush().unwrap();
    }

    #[test]
    fn cross_language_conformance() {
        let Ok(helper) = std::env::var("LAGHU_CROSS_C_HELPER") else {
            return;
        };
        let root = cross_language_root("cross-language");
        let queue_path = root.join("jobs.queue");
        let cache_path = root.join("cache");
        let signal = root.join("registry");
        fs::create_dir_all(&root).unwrap();

        let mut queue = Queue::create(&queue_path).unwrap();
        publish_cross_language_job(&mut queue);
        let status = std::process::Command::new(&helper)
            .arg("--consume-rust-job")
            .arg(&queue_path)
            .status()
            .unwrap();
        assert!(status.success());
        drop(queue);

        let mut peer = std::process::Command::new(&helper)
            .arg("--registry-peer")
            .arg(&cache_path)
            .arg(&signal)
            .spawn()
            .unwrap();
        cross_language_wait(&cross_language_signal(&signal, "ready"));

        let queue = Queue::create(&queue_path).unwrap();
        let mut registry = OperationalRegistry::open(&cache_path).unwrap();
        registry.heartbeat(&queue);
        registry.record(1_200, Some(OPERATIONAL_FAILURE_TRANSFORM));
        fs::write(cross_language_signal(&signal, "healthy"), b"ok").unwrap();
        cross_language_wait(&cross_language_signal(&signal, "healthy.ok"));

        registry.update(|slot| {
            write_u64(slot, OPERATIONAL_SLOT_HEARTBEAT_OFFSET as usize, 1);
        });
        fs::write(cross_language_signal(&signal, "stale"), b"ok").unwrap();
        cross_language_wait(&cross_language_signal(&signal, "stale.ok"));

        drop(registry);
        fs::write(cross_language_signal(&signal, "released"), b"ok").unwrap();
        cross_language_wait(&cross_language_signal(&signal, "released.ok"));

        let mut replacement = OperationalRegistry::open(&cache_path).unwrap();
        replacement.heartbeat(&queue);
        fs::write(cross_language_signal(&signal, "replaced"), b"ok").unwrap();
        cross_language_wait(&cross_language_signal(&signal, "replaced.ok"));
        drop(replacement);
        assert!(peer.wait().unwrap().success());
        drop(queue);
        fs::remove_dir_all(root).unwrap();
    }

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
    fn emits_external_map_without_source_content() {
        let result = transform_artifact(
            "function publicName(longLocal){return longLocal+1}",
            "laghu-fixture.js",
            false,
            "last 2 chrome versions",
            true,
        )
        .unwrap();
        let map = String::from_utf8(result.source_map).unwrap();
        assert!(map.contains("\"version\":3"));
        assert!(map.contains("laghu-fixture.js"));
        assert!(!map.contains("sourcesContent"));
    }

    #[test]
    fn defer_certificate_rejects_parser_sensitive_programs() {
        let safe = transform(
            "function safeDeferredScript(veryLongArgumentName){const veryLongLocalName=veryLongArgumentName+veryLongArgumentName;return veryLongLocalName+veryLongLocalName;}console.log(safeDeferredScript(1));",
            false,
            "last 2 chrome versions",
        )
        .unwrap()
        .2;
        assert_eq!(safe & FLAG_DEFER_SAFE, FLAG_DEFER_SAFE);
        let writer = transform(
            "document.write( '<p>parser sensitive</p>' ); function publicParserHelper(veryLongLocalName) { const anotherVeryLongLocalName = veryLongLocalName + 1; return anotherVeryLongLocalName + anotherVeryLongLocalName; } console.log(publicParserHelper(1));",
            false,
            "last 2 chrome versions",
        )
        .unwrap()
        .2;
        assert_eq!(writer & FLAG_DEFER_SAFE, 0);
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
        let source = b"function publicName(longLocal){ const repeatedValue = longLocal + 1; console.log(repeatedValue, repeatedValue, repeatedValue, repeatedValue); return repeatedValue + repeatedValue + repeatedValue; }";
        let source_hash = sha256(source);
        let index_key = sha256(b"test-index");
        let base = QUEUE_HEADER_SIZE as usize;
        write_u32(
            &mut queue.map,
            base + QUEUE_SLOT_STATE_OFFSET as usize,
            QUEUE_SLOT_READY,
        );
        write_u32(
            &mut queue.map,
            base + QUEUE_SLOT_KIND_OFFSET as usize,
            JOB_JAVASCRIPT,
        );
        write_u64(
            &mut queue.map,
            base + QUEUE_SLOT_PAYLOAD_LENGTH_OFFSET as usize,
            source.len() as u64,
        );
        write_u64(
            &mut queue.map,
            base + QUEUE_SLOT_FILTERS_OFFSET as usize,
            FILTER_SOURCE_MAP,
        );
        write_fixed(
            &mut queue.map[base + QUEUE_SLOT_INDEX_KEY_OFFSET as usize
                ..base + QUEUE_SLOT_REQUEST_PATH_OFFSET as usize],
            &index_key,
        )
        .unwrap();
        write_fixed(
            &mut queue.map[base + QUEUE_SLOT_REQUEST_PATH_OFFSET as usize
                ..base + QUEUE_SLOT_VALIDATOR_OFFSET as usize],
            "/app.js",
        )
        .unwrap();
        write_fixed(
            &mut queue.map[base + QUEUE_SLOT_VALIDATOR_OFFSET as usize
                ..base + QUEUE_SLOT_CONTENT_TYPE_OFFSET as usize],
            &source_hash,
        )
        .unwrap();
        write_fixed(
            &mut queue.map[base + QUEUE_SLOT_POLICY_KEY_OFFSET as usize
                ..base + QUEUE_SLOT_PROVIDER_ID_OFFSET as usize],
            &sha256(b"policy"),
        )
        .unwrap();
        write_fixed(
            &mut queue.map[base + QUEUE_SLOT_JAVASCRIPT_TARGET_OFFSET as usize
                ..base + QUEUE_SLOT_FILTERS_OFFSET as usize],
            "last 2 chrome versions",
        )
        .unwrap();
        queue.map[base + QUEUE_SLOT_HEADER_SIZE as usize
            ..base + QUEUE_SLOT_HEADER_SIZE as usize + source.len()]
            .copy_from_slice(source);
        queue.map.flush().unwrap();
        assert!(matches!(
            process_one(&mut queue, &cache),
            ProcessOutcome::Success
        ));
        assert!(cache.join(format!("index-{index_key}.meta")).is_file());
        assert!(fs::read_dir(&cache).unwrap().any(|entry| {
            let path = entry.unwrap().path();
            path.file_name()
                .is_some_and(|name| name.to_string_lossy().starts_with("variant-"))
                && fs::read(path).is_ok_and(|bytes| bytes.starts_with(b"{\"version\":3"))
        }));
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

    #[test]
    fn advertises_queue_once_per_second() {
        let root = std::env::temp_dir().join(format!(
            "laghu-js-advertise-test-{}-{}",
            std::process::id(),
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        let queue_path = root.join("javascript.queue");
        fs::create_dir_all(&root).unwrap();
        let mut queue = Queue::create(&queue_path).unwrap();
        queue.advertise().unwrap();
        let advertised = queue.last_advertised;
        queue.advertise().unwrap();
        assert_eq!(queue.last_advertised, advertised);
        assert_eq!(
            read_u32(&queue.map, QUEUE_HEADER_CAPABILITIES_OFFSET as usize),
            1
        );
        assert_eq!(
            &queue.map[QUEUE_HEADER_BACKEND_ID_OFFSET as usize
                ..QUEUE_HEADER_BACKEND_ID_OFFSET as usize + BACKEND.len()],
            BACKEND.as_bytes()
        );
        drop(queue);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn publishes_operational_v4_heartbeat_metrics_and_release() {
        let root = std::env::temp_dir().join(format!(
            "laghu-js-operational-test-{}-{}",
            std::process::id(),
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        let cache = root.join("cache");
        let queue_path = root.join("javascript.queue");
        fs::create_dir_all(&root).unwrap();
        let queue = Queue::create(&queue_path).unwrap();
        let mut registry = OperationalRegistry::open(&cache).unwrap();
        registry.heartbeat(&queue);
        registry.record(1_200, Some(OPERATIONAL_FAILURE_TRANSFORM));
        let base = operational_slot_base(registry.slot);
        assert_eq!(
            read_u64(&registry.map, OPERATIONAL_HEADER_MAGIC_OFFSET as usize),
            OPERATIONAL_MAGIC
        );
        assert_eq!(
            read_u32(&registry.map, OPERATIONAL_HEADER_VERSION_OFFSET as usize),
            OPERATIONAL_VERSION
        );
        assert_eq!(
            read_u64(
                &registry.map,
                base + OPERATIONAL_SLOT_SEQUENCE_OFFSET as usize
            ) & 1,
            0
        );
        assert_eq!(
            read_u32(
                &registry.map,
                base + OPERATIONAL_SLOT_ACTIVE_OFFSET as usize
            ),
            1
        );
        assert_eq!(
            read_u32(
                &registry.map,
                base + OPERATIONAL_SLOT_PROCESS_KIND_OFFSET as usize
            ),
            2
        );
        assert_eq!(
            read_u64(
                &registry.map,
                base + OPERATIONAL_SLOT_QUEUE_CAPACITY_OFFSET as usize
            ),
            8
        );
        assert_eq!(
            read_u64(
                &registry.map,
                base + OPERATIONAL_SLOT_QUEUE_OCCUPIED_OFFSET as usize
            ),
            0
        );
        assert_eq!(
            read_u64(
                &registry.map,
                base + OPERATIONAL_SLOT_LATENCY_COUNT_OFFSET as usize
            ),
            1
        );
        assert_eq!(
            read_u64(
                &registry.map,
                base + OPERATIONAL_SLOT_FAILURES_OFFSET as usize
                    + OPERATIONAL_FAILURE_TRANSFORM * 8
            ),
            1
        );
        drop(registry);
        let bytes = fs::read(operational_path(&cache)).unwrap();
        assert_eq!(
            read_u32(&bytes, base + OPERATIONAL_SLOT_ACTIVE_OFFSET as usize),
            0
        );
        assert_eq!(bytes[base + OPERATIONAL_SLOT_HEALTHY_OFFSET as usize], 0);
        drop(queue);
        fs::remove_file(operational_path(&cache)).unwrap();
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn incompatible_operational_registry_is_explicitly_unavailable() {
        let root = std::env::temp_dir().join(format!(
            "laghu-js-operational-unavailable-test-{}-{}",
            std::process::id(),
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        fs::create_dir_all(&root).unwrap();
        fs::write(operational_path(&root), b"incompatible").unwrap();
        assert!(matches!(Operational::open(&root), Operational::Unavailable));
        fs::remove_file(operational_path(&root)).unwrap();
        fs::remove_dir_all(root).unwrap();
    }
}
