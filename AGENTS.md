# yunluo candidate responsibilities

Read porting.toml first. Read the matching current UniXNU guide before implementation; update each owning area's findings/evidence with code changes. This format-2 candidate is not a complete working-runtime migration. Preserve Mu in boot and UniXNU in cust; never infer a partition GUID/offset/capacity. Source host tests do not prove hardware behavior. No flash, format, device boot, deletion, or history rewrite is authorized here.

An initial public WIP is authorized for source review, not as a usable-system
release. Before a completed-port claim, follow README.md, the upstream record
checker and --base check, component/source/privacy review, clean image build
and the corresponding device observations. Code and docs use separate commits.
