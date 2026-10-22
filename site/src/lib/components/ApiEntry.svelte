<script>
	// One declaration of FMinus.h and what it holds, in the header's order: types, namespaces and Program's
	// functions as sections; a struct's or a namespace's fields and functions as a list.
	import ApiEntry from './ApiEntry.svelte';

	let { entry, depth = 2 } = $props();

	const compactKinds = new Set(['field', 'alias', 'function']);
	const compactIn = (kind) => kind === 'struct' || kind === 'union' || kind === 'namespace';

	// Members in a list are runs: a field and the fields after it that share its comment (minimum, maximum), shown
	// as one entry's terms with the comment once.
	const groups = $derived.by(() => {
		const out = [];
		for (const child of entry.children) {
			const compact = compactIn(entry.kind) && compactKinds.has(child.kind);
			const last = out[out.length - 1];
			if (compact && last?.runs) {
				const run = last.runs[last.runs.length - 1];
				if (child.sharedWith && run.items[0].id === child.sharedWith) run.items.push(child);
				else last.runs.push({ items: [child] });
			} else if (compact) out.push({ runs: [{ items: [child] }] });
			else out.push({ section: child });
		}
		return out;
	});
	const level = $derived(Math.min(depth, 6));
	const title = $derived(entry.kind === 'function' ? `${entry.label}()` : entry.label);
</script>

<section class="decl decl-{entry.kind}" id={entry.id}>
	<svelte:element this={`h${level}`} class="decl-title">
		<span class="kind">{entry.kindLabel}</span>
		<code>{title}</code>
		<a class="src" href={entry.source} title="This declaration in FMinus.h on GitHub">source</a>
	</svelte:element>

	{#if entry.codeHtml}
		{@html entry.codeHtml}
	{:else if entry.signatures.length && !['class', 'struct', 'union'].includes(entry.kind)}
		{#each entry.signatures as sig (sig.line)}
			<pre class="signature">{@html sig.html}</pre>
		{/each}
	{/if}

	{#if entry.docHtml}
		<div class="doc">{@html entry.docHtml}</div>
	{:else if entry.note}
		<p class="note">
			{#if entry.note.href}<a href={entry.note.href}>{entry.note.text}</a>{:else}{entry.note.text}{/if}.
		</p>
	{/if}

	{#if entry.enumerators.some((e) => e.docHtml)}
		<dl class="members enumerators">
			{#each entry.enumerators as e (e.id)}
				<dt id={e.id}><code>{e.name}{#if e.value} = {e.value}{/if}</code></dt>
				<dd>{#if e.docHtml}<div class="doc">{@html e.docHtml}</div>{/if}</dd>
			{/each}
		</dl>
	{:else if entry.enumerators.length}
		<p class="enumerator-list">
			<span class="muted">Enumerators:</span>
			{#each entry.enumerators as e (e.id)}<code id={e.id}>{e.name}{#if e.value} = {e.value}{/if}</code>{' '}{/each}
		</p>
	{/if}

	{#each groups as group, i (group.section?.id ?? `members-${i}`)}
		{#if group.runs}
			<dl class="members">
				{#each group.runs as run (run.items[0].id)}
					{#each run.items as m, k (m.id)}
						<dt id={m.id} class:shared={k > 0}>
							{#if m.codeHtml}
								{@html m.codeHtml}
							{:else}
								{#each m.signatures as sig (sig.line)}
									<div class="member-sig">{@html sig.html}</div>
								{/each}
							{/if}
							<a class="src" href={m.source} title="This declaration in FMinus.h on GitHub">source</a>
						</dt>
					{/each}
					{@const m = run.items[0]}
					<dd>
						{#if m.docHtml}
							<div class="doc">{@html m.docHtml}</div>
						{:else if m.note}
							<p class="note">
								{#if m.note.href}<a href={m.note.href}>{m.note.text}</a>{:else}{m.note.text}{/if}.
							</p>
						{/if}
					</dd>
				{/each}
			</dl>
		{:else}
			<ApiEntry entry={group.section} depth={depth + 1} />
		{/if}
	{/each}
</section>

<style>
	.decl {
		margin: 0 0 1.75rem;
	}

	.decl :global(.decl) {
		margin-left: 0;
	}

	.decl-title {
		display: flex;
		flex-wrap: wrap;
		align-items: baseline;
		gap: 0.25rem 0.6rem;
		scroll-margin-top: calc(var(--header) + 0.75rem);
	}

	.decl-title code {
		font-size: 0.95em;
		background: none;
		padding: 0;
		overflow-wrap: anywhere;
	}

	h2.decl-title {
		border-bottom: 1px solid var(--border-soft);
		margin-top: 2.5rem;
	}

	.kind {
		font-size: 0.75rem;
		font-weight: 500;
		text-transform: uppercase;
		letter-spacing: 0.04em;
		color: var(--muted);
	}

	.src {
		margin-left: auto;
		font-size: 0.75rem;
		font-weight: 400;
		color: var(--muted);
	}

	.signature {
		white-space: pre-wrap;
		overflow-wrap: anywhere;
	}

	.signature :global(code.shiki-inline) {
		background: none;
		padding: 0;
	}

	.note {
		color: var(--muted);
		font-size: 0.9rem;
		font-style: italic;
	}

	.members {
		margin: 0.75rem 0 1.5rem;
		border-top: 1px solid var(--border-soft);
	}

	.members dt {
		display: flex;
		align-items: baseline;
		flex-wrap: wrap;
		gap: 0.5rem;
		padding-top: 0.65rem;
		scroll-margin-top: calc(var(--header) + 0.75rem);
	}

	.members dt.shared {
		padding-top: 0.1rem;
	}

	.members dt :global(pre) {
		flex: 1 1 100%;
		margin: 0;
	}

	.member-sig {
		font-family: var(--font-mono);
		font-size: 0.875rem;
		min-width: 0;
		overflow-wrap: anywhere;
	}

	.member-sig :global(code.shiki-inline) {
		background: none;
		padding: 0;
		font-size: 1em;
	}

	.members dd {
		margin: 0.25rem 0 0 1.25rem;
		padding-bottom: 0.65rem;
		border-bottom: 1px solid var(--border-soft);
	}

	.members dd .note {
		margin: 0;
	}

	.members dd:empty {
		padding-bottom: 0.35rem;
	}

	.enumerator-list {
		font-size: 0.9rem;
	}

	.enumerator-list code {
		margin-right: 0.35rem;
		scroll-margin-top: calc(var(--header) + 0.75rem);
	}

	.enumerators dt code {
		font-size: 0.875rem;
	}
</style>
