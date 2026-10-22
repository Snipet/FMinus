<script>
	import { page } from '$app/state';
	import { resolve } from '$app/paths';
</script>

<svelte:head>
	<title>{page.status} · FMinus</title>
</svelte:head>

<main id="content" class="page-single">
	<h1>{page.status === 404 ? 'Not found' : `Error ${page.status}`}</h1>
	<p>{page.error?.message ?? ''}</p>
	<p><a href={resolve('/')}>FMinus's documentation</a></p>
</main>
