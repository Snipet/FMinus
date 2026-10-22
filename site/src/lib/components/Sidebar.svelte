<script>
	// A page's sidebar: always shown beside the page on a wide screen, behind a "Contents" button on a narrow one.
	import { afterNavigate } from '$app/navigation';

	let { label = 'Contents', children } = $props();
	let open = $state(false);

	afterNavigate(() => {
		open = false;
	});
</script>

<aside class="sidebar" class:open aria-label={label}>
	<button class="sidebar-toggle" type="button" aria-expanded={open} onclick={() => (open = !open)}>{label}</button>
	<div class="sidebar-body">
		{@render children()}
	</div>
</aside>
