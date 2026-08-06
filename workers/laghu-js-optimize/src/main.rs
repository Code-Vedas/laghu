// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

use std::{
    fs,
    io::{self, Read, Write},
    path::{Path, PathBuf},
    thread,
    time::{Duration, SystemTime},
};

#[cfg(test)]
use std::time::UNIX_EPOCH;

use anyhow::{Context, Result, anyhow, bail};
use browserslist::{Opts, resolve};
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

mod publication;
mod queue;
#[cfg(test)]
use publication::write_fixed;
use publication::{publish, publish_catalog};
use queue::Queue;
#[cfg(test)]
use queue::{FILTER_SOURCE_MAP, JOB_JAVASCRIPT, queue_slot_base};

mod operational;
#[cfg(test)]
use operational::OperationalRegistry;
use operational::{
    FAILURE_CACHE as OPERATIONAL_FAILURE_CACHE, FAILURE_QUEUE as OPERATIONAL_FAILURE_QUEUE,
    FAILURE_TRANSFORM as OPERATIONAL_FAILURE_TRANSFORM, Operational,
};

const MAX_INPUT: usize = 2 * 1024 * 1024;
const DEFAULT_TARGET: &str = "defaults and supports es6-module and not dead";
const BACKEND: &str = "laghu-swc-75.0.0-js-v3";
const FLAG_MODULE: u32 = 1;
const FLAG_URL_INDEPENDENT: u32 = 2;
const FLAG_CONCAT_SAFE: u32 = 4;
const FLAG_TOP_LEVEL_DECLARATION_FREE: u32 = 8;
const FLAG_DEFER_SAFE: u32 = 16;
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

pub(crate) fn c_string(bytes: &[u8]) -> Result<&str> {
    let end = bytes
        .iter()
        .position(|byte| *byte == 0)
        .context("unterminated queue string")?;
    std::str::from_utf8(&bytes[..end]).context("queue string is not UTF-8")
}

pub(crate) fn read_u32(bytes: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(
        bytes[offset..offset + 4]
            .try_into()
            .expect("fixed queue layout"),
    )
}

pub(crate) fn read_u64(bytes: &[u8], offset: usize) -> u64 {
    u64::from_le_bytes(
        bytes[offset..offset + 8]
            .try_into()
            .expect("fixed queue layout"),
    )
}

pub(crate) fn write_u32(bytes: &mut [u8], offset: usize, value: u32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

pub(crate) fn write_u64(bytes: &mut [u8], offset: usize, value: u64) {
    bytes[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}

pub(crate) fn sha256(data: &[u8]) -> String {
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
        let (capacity, occupied) = queue.snapshot();
        operational.heartbeat(capacity, occupied);
        let started = SystemTime::now();
        let processed = process_one(&mut queue, &cache_path);
        let elapsed = started.elapsed().map_or(0, |time| time.as_micros() as u64);
        record_process(&mut operational, &processed, elapsed);
        return Ok(());
    }
    let mut operational = Operational::open(&cache_path);
    loop {
        let (capacity, occupied) = queue.snapshot();
        operational.heartbeat(capacity, occupied);
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
        registry.heartbeat(8, 0);
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
        replacement.heartbeat(8, 0);
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
}
