<script>
	import { flushSync } from 'svelte';
	import { afterNavigate } from '$app/navigation';
	import g from '$lib/generated/grammar.json';
	import Sidebar from '$lib/components/Sidebar.svelte';

	let filter = $state('');
	const query = $derived(filter.trim().toLowerCase());
	const unquote = (text) => text.replace(/^"(.*)"$/, '$1').toLowerCase();

	/** A rule matches by its name, or by a symbol it uses, spelled as the grammar spells it ("if", IDENTIFIER). */
	function matches(rule) {
		if (!query) return true;
		if (rule.name.toLowerCase().includes(query)) return true;
		const q = unquote(query);
		return rule.alternatives.some((a) =>
			a.items.some((s) => (s.name && s.name.toLowerCase() === q) || (s.text && unquote(s.text) === q)),
		);
	}
	const shown = $derived(g.rules.filter(matches).length);

	/**
	 * A link to a rule the filter hides shows it: the filter is cleared first, so the rule is there to scroll to.
	 * Returns whether it was hidden.
	 */
	function reveal(hash) {
		if (!query || !hash?.startsWith('#rule-')) return false;
		const name = decodeURIComponent(hash.slice('#rule-'.length));
		const rule = g.rules.find((r) => r.name === name);
		if (!rule || matches(rule)) return false;
		flushSync(() => {
			filter = '';
		});
		return true;
	}

	/** Before the browser follows a link on this page (a cross-link, the sidebar, a search result, a diagram's box). */
	function revealOnClick(event) {
		const a = event.target instanceof Element ? event.target.closest('a[href]') : null;
		const href = a?.getAttribute('href');
		if (!href) return;
		const url = new URL(href, location.href);
		if (url.pathname === location.pathname) reveal(url.hash);
	}

	// a navigation that reached a hidden rule some other way (the search box's Enter, history)
	afterNavigate(({ to }) => {
		const hash = to?.url.hash;
		if (reveal(hash)) document.getElementById(decodeURIComponent(hash.slice(1)))?.scrollIntoView();
	});

	const assoc = { left: 'left to right', right: 'right to left', nonassoc: 'none', precedence: 'precedence only' };
	const tokenByName = new Map(g.tokens.map((t) => [t.name, t]));
</script>

<svelte:window onclickcapture={revealOnClick} />

<svelte:head>
	<title>Grammar · FMinus</title>
	<meta name="description" content={g.summary} />
</svelte:head>

{#snippet symbol(s)}
	{#if s.kind === 'rule'}
		<a class="sym nt" href={`#rule-${s.name}`}>{s.name}</a>
	{:else if s.kind === 'token'}
		<a class="sym t" href={`#token-${s.name}`}>{s.text}</a>
	{:else if s.kind === 'mid'}
		<span class="sym mid" title="A mid-rule action (C++), left out">{'{…}'}</span>
	{:else if s.kind === 'empty'}
		<span class="sym empty">%empty</span>
	{:else}
		<span class="sym">{s.name}</span>
	{/if}
{/snippet}

{#snippet ruleList(names)}
	{#each names as name, i (name)}{#if i}, {/if}<a href={`#rule-${name}`}><code>{name}</code></a>{/each}
{/snippet}

<div class="page-grid">
	<Sidebar label="Grammar">
		<p class="heading">Grammar</p>
		<ul>
			<li><a href="#overview">Overview</a></li>
			<li><a href="#rules">Rules</a></li>
		</ul>
		<ul class="code rules-nav">
			{#each g.rules as rule (rule.id)}
				<li><a href={`#${rule.id}`}>{rule.name}</a></li>
			{/each}
		</ul>
		<ul>
			<li><a href="#tokens">Tokens</a></li>
			<li><a href="#precedence">Precedence</a></li>
			<li><a href="#scanner">The scanner</a></li>
			<li><a href="#declarations">Bison's declarations</a></li>
		</ul>
	</Sidebar>

	<main id="content" class="page-main">
		<header class="page-head">
			<h1>Grammar</h1>
			<p class="from">
				Generated from <a href={g.parser.url}><code>{g.parser.file}</code></a> and
				<a href={g.lexer.url}><code>{g.lexer.file}</code></a>: the rules without their C++ actions.
			</p>
		</header>

		<section id="overview">
			<h2>Overview</h2>
			<div class="doc summary">{@html g.summaryHtml}</div>
		</section>

		<section id="rules">
			<h2>Rules</h2>
			<div class="filter">
				<input
					type="search"
					bind:value={filter}
					placeholder="Filter the rules by name, or by a symbol they use"
					aria-label="Filter the rules by name, or by a symbol they use"
				/>
				<span class="muted count" aria-live="polite">{shown} of {g.rules.length} rules</span>
			</div>
			<p class="muted small">
				Each rule as Parser.y writes it, and as a railroad diagram, which draws a left-recursive list as a loop.
				Boxes with round ends are tokens; square ones are rules.
			</p>

			{#each g.rules as rule (rule.id)}
				<article class="rule" id={rule.id} hidden={!matches(rule)}>
					<h3 class="rule-title">
						<code>{rule.name}</code>
						{#if rule.type}<span class="type" title="Its semantic value's type">&lt;{rule.type}&gt;</span>{/if}
						<a class="src" href={rule.source}>source</a>
					</h3>
					{#if rule.docHtml}<div class="doc">{@html rule.docHtml}</div>{/if}
					<div class="railroad-wrap">{@html rule.diagram}</div>
					<div class="bnf">
						<div class="lhs"><a href={`#${rule.id}`}>{rule.name}</a></div>
						{#each rule.alternatives as alt, i (i)}
							<div class="alt">
								<span class="sep">{i === 0 ? ':' : '|'}</span>
								<span class="items">
									{#each alt.items as s, k (k)}{@render symbol(s)}{/each}
									{#if alt.prec}
										<a class="sym prec" href={`#${alt.prec.id}`} title="This alternative's precedence">%prec {alt.prec.text}</a>
									{/if}
								</span>
							</div>
							{#if alt.docHtml || alt.actionHtml}
								<div class="alt-doc">
									{#if alt.docHtml}<div class="doc">{@html alt.docHtml}</div>{/if}
									{#if alt.actionHtml}<div class="doc action-doc"><span class="muted">In its C++ action:</span> {@html alt.actionHtml}</div>{/if}
								</div>
							{/if}
						{/each}
						<div class="alt"><span class="sep">;</span></div>
					</div>
					{#if rule.usedBy.length}
						<p class="used-by muted">Used by {@render ruleList(rule.usedBy)}.</p>
					{:else}
						<p class="used-by muted">The start rule.</p>
					{/if}
				</article>
			{/each}
		</section>

		<section id="tokens">
			<h2>Tokens</h2>
			<p class="muted small">
				Every <code>%token</code> of Parser.y, its spelling, its value, and the patterns of Lexer.l that return it.
			</p>
			<div class="table-wrap">
				<table class="tokens">
					<thead>
						<tr><th>Token</th><th>Spelling</th><th>Value</th><th>Lexer.l</th><th>Precedence</th><th>Used by</th></tr>
					</thead>
					<tbody>
						{#each g.tokens as t (t.id)}
							<tr id={t.id}>
								<td><code>{t.name}</code></td>
								<td>{#if t.alias}<code>{t.alias}</code>{/if}</td>
								<td>{#if t.type}<code>{t.type}</code>{/if}</td>
								<td class="patterns">
									{#each t.patterns as p (p.id)}
										<a href={`#${p.id}`}><code>{#if p.conditions.length}&lt;{p.conditions.join(',')}&gt;{/if}{@html p.patternHtml.replace(/<a [^>]*>|<\/a>/g, '')}</code></a>
									{:else}
										<span class="muted">none</span>
									{/each}
								</td>
								<td>{#if t.level}<a href={`#prec-${t.level}`}>{t.level}</a>{/if}</td>
								<td class="users">{@render ruleList(t.usedBy)}</td>
							</tr>
						{/each}
					</tbody>
				</table>
			</div>
		</section>

		<section id="precedence">
			<h2>Precedence</h2>
			{#each g.precedence.filter((p) => p.docHtml) as p (p.id)}
				<div class="doc">{@html p.docHtml}</div>
			{/each}
			<p class="muted small">From the loosest to the tightest: a later line binds tighter, as in Parser.y.</p>
			<div class="table-wrap">
				<table>
					<thead><tr><th>Level</th><th>Associativity</th><th>Tokens</th></tr></thead>
					<tbody>
						{#each g.precedence as p (p.id)}
							<tr id={p.id}>
								<td><a href={p.source}>{p.level}</a></td>
								<td>{assoc[p.assoc] ?? p.assoc} <span class="muted">(<code>%{p.assoc}</code>)</span></td>
								<td>
									{#each p.symbols as s, i (s.name)}{#if i}{' '}{/if}{#if s.precedenceOnly}<code title="A name for %prec only">{s.text}</code>{#if s.usedBy.length}<span class="muted"> (in {@render ruleList(s.usedBy)})</span>{/if}{:else}<a href={`#token-${s.name}`}><code>{s.text}</code></a>{/if}{/each}
								</td>
							</tr>
						{/each}
					</tbody>
				</table>
			</div>
		</section>

		<section id="scanner">
			<h2>The scanner</h2>
			<div class="doc">{@html g.lexerHtml}</div>
			<p class="small">
				<span class="muted">Options:</span>
				{#each g.lexer.options as o, i (o)}{#if i}{' '}{/if}<code>{o}</code>{/each}.
				{#if g.lexer.conditions.length}
					<span class="muted">Start conditions:</span>
					{#each g.lexer.conditions as c, i (c.name)}{#if i}, {/if}<code>{c.name}</code> ({c.exclusive ? 'exclusive' : 'inclusive'}){/each}.
				{/if}
			</p>

			<h3 id="scanner-definitions">Definitions</h3>
			<div class="table-wrap">
				<table>
					<thead><tr><th>Name</th><th>Pattern</th></tr></thead>
					<tbody>
						{#each g.definitions as d (d.id)}
							<tr id={d.id}><td><a href={d.source}><code>{d.name}</code></a></td><td><code>{@html d.patternHtml}</code></td></tr>
						{/each}
					</tbody>
				</table>
			</div>

			<h3 id="scanner-rules">Rules, in order</h3>
			<p class="muted small">Flex takes the longest match, and of equal ones the earliest rule.</p>
			<div class="table-wrap">
				<table class="lex">
					<thead><tr><th>#</th><th>Pattern</th><th>Result</th></tr></thead>
					<tbody>
						{#each g.lexRules as r, i (r.id)}
							<tr id={r.id}>
								<td><a href={r.source}>{i + 1}</a></td>
								<td>
									{#if r.conditions.length}<code class="cond">&lt;{r.conditions.join(',')}&gt;</code>{/if}
									<code>{@html r.patternHtml}</code>
								</td>
								<td>
									{#if r.outcome.kind === 'tokens'}
										{#each r.outcome.tokens as name, k (name)}{#if k}<span class="muted"> or </span>{/if}{#if tokenByName.has(name)}<a href={`#token-${name}`}><code>{name}</code></a>{:else if name === 'YYEOF'}<span>the end of the input (<code>YYEOF</code>)</span>{:else}<code>{name}</code>{/if}{/each}{#if r.outcome.resumes}<span class="muted">; at an included file's end, back to the file that included it</span>{/if}
									{:else if r.outcome.kind === 'include'}
										<span>a directive: <code>#include "file"</code> reads the file from here; <code>#pragma</code> is ignored</span>
									{:else if r.outcome.kind === 'error'}
										<span class="error">error: {r.outcome.message}</span>
									{:else if r.outcome.kind === 'begin'}
										<span>enters <code>{r.outcome.condition}</code></span>
									{:else}
										<span class="muted">skipped</span>
									{/if}
								</td>
							</tr>
						{/each}
					</tbody>
				</table>
			</div>
		</section>

		<section id="declarations">
			<h2>Bison's declarations</h2>
			<p class="muted small">As Parser.y writes them, comments included, without the C++ of its <code>%code</code> blocks.</p>
			{@html g.declarationsHtml}
		</section>
	</main>
</div>

<style>
	.filter {
		display: flex;
		flex-wrap: wrap;
		align-items: center;
		gap: 0.5rem 1rem;
		margin: 0.5rem 0 0.75rem;
		position: sticky;
		top: var(--header);
		background: var(--bg);
		padding: 0.5rem 0;
		z-index: 5;
	}

	.filter input {
		flex: 1 1 18rem;
		min-width: 0;
		font: inherit;
		font-size: 0.95rem;
		color: var(--fg);
		background: var(--bg-soft);
		border: 1px solid var(--border);
		border-radius: 6px;
		padding: 0.4rem 0.65rem;
	}

	.count {
		font-size: 0.875rem;
	}

	.small {
		font-size: 0.9rem;
	}

	.summary :global(pre.ebnf) {
		font-size: 0.78rem;
		line-height: 1.55;
	}

	.summary :global(pre.ebnf a) {
		color: inherit;
	}

	.summary :global(.ebnf-lhs) {
		font-weight: 600;
	}

	.summary :global(a.ebnf-rule),
	.summary :global(a.ebnf-lhs) {
		color: var(--link);
	}

	.summary :global(.ebnf-terminal),
	.summary :global(.ebnf-token) {
		color: var(--fg-soft);
	}

	.summary :global(.ebnf-note) {
		color: var(--muted);
		font-style: italic;
	}

	.summary :global(.ebnf-op) {
		color: var(--muted);
	}

	.rule {
		border-top: 1px solid var(--border-soft);
		padding: 0.5rem 0 1rem;
		scroll-margin-top: calc(var(--header) + 3.75rem);
	}

	.rule-title {
		display: flex;
		flex-wrap: wrap;
		align-items: baseline;
		gap: 0.25rem 0.75rem;
		margin: 0.75rem 0 0.5rem;
	}

	.rule-title code {
		background: none;
		padding: 0;
		font-size: 1.05em;
	}

	.type {
		font-family: var(--font-mono);
		font-size: 0.8rem;
		font-weight: 400;
		color: var(--muted);
	}

	.src {
		margin-left: auto;
		font-size: 0.75rem;
		font-weight: 400;
		color: var(--muted);
	}

	.bnf {
		font-family: var(--font-mono);
		font-size: 0.8125rem;
		line-height: 1.6;
		background: var(--code-bg);
		border: 1px solid var(--border-soft);
		border-radius: 6px;
		padding: 0.6rem 1rem;
		margin: 0 0 0.75rem;
	}

	.bnf .lhs a {
		color: var(--fg);
		font-weight: 600;
	}

	.alt {
		display: flex;
		gap: 0.6rem;
		padding-left: 1.5rem;
	}

	.sep {
		color: var(--muted);
		flex: 0 0 auto;
	}

	.items {
		display: flex;
		flex-wrap: wrap;
		gap: 0 0.6rem;
		min-width: 0;
	}

	.sym {
		overflow-wrap: anywhere;
	}

	.sym.nt {
		color: var(--link);
	}

	.sym.t {
		color: var(--fg-soft);
	}

	.sym.mid,
	.sym.empty,
	.sym.prec {
		color: var(--muted);
		font-style: italic;
	}

	.used-by {
		font-size: 0.875rem;
		margin: 0;
	}

	.tokens td,
	.lex td {
		font-size: 0.875rem;
	}

	.patterns a + a::before {
		content: ' ';
	}

	.patterns a {
		display: inline-block;
		margin: 0 0.35rem 0.2rem 0;
	}

	.users {
		min-width: 12rem;
	}

	.cond {
		color: var(--muted);
		margin-right: 0.25rem;
	}

	.error {
		color: var(--fg-soft);
	}

	/* an alternative's comments, under it, in the prose's face */
	.alt-doc {
		font-family: var(--font-sans);
		font-size: 0.875rem;
		line-height: 1.5;
		color: var(--fg-soft);
		padding: 0 0 0.35rem 2.6rem;
	}

	.alt-doc :global(p) {
		margin: 0.1rem 0;
	}

	.action-doc > .muted {
		display: block;
		font-size: 0.8125rem;
	}
</style>
