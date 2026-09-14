# The project website

[saworbit.github.io/didi](https://saworbit.github.io/didi/) is built from this
directory by [the Pages workflow](../.github/workflows/pages.yml) on every push
to `main`, and checked on every pull request. No branch carries generated HTML.

```
site/
├── build.py        the generator: stdlib only, verifies its own output
├── templates/      one file per page: index.html, brand.html, 404.html
├── partials/       head, nav and footer, shared by every page
└── static/         copied to the site root as-is (site.css)
```

The brand assets are not copied here. `build.py` inlines the SVG sources from
[`docs/brand/svg`](../docs/brand/BRAND.md) into the pages, so a mark on the
site is the same bytes as the mark in the addon and in the README, and follows
the reader's colour scheme through `currentColor`. The PNG exports are copied
from `docs/brand/png` at build time and served under `brand/`.

The version and the surface counts on the site are read from `CMakeLists.txt`
and the README status block, which `tools/validate_documentation.py` already
keeps aligned with the built binary. The site cannot publish a number the
README does not. If either file stops matching, the build fails rather than
guessing.

## Building locally

```bash
python site/build.py --out _site    # writes the site; _site/ is ignored by git
python site/build.py --check        # builds into a temporary directory and verifies only
```

Then open `_site/index.html`, or serve it with any static file server. The
build refuses to empty an output directory it did not write.

## Placeholders

The templates are plain HTML with a handful of tokens:

| Token | Expands to |
| :--- | :--- |
| `{{partial:NAME}}` | `partials/NAME.html`, expanded before anything else |
| `{{svg:NAME}}` | `docs/brand/svg/NAME.svg`, inlined verbatim |
| `{{svg:NAME\|ATTRS}}` | the same, with `ATTRS` added to the root `<svg>` tag |
| `{{root}}` | the path from the page back to the site root |
| `{{version}}` | the project version from `CMakeLists.txt` |
| `{{canonical}}` `{{implemented}}` `{{unimplemented}}` `{{legacy}}` `{{total}}` | the surface counts from the README |
| `{{tests}}` | the README tests badge |
| `{{title}}` `{{description}}` `{{canonical_url}}` | per page, declared in `build.py` |

A placeholder that does not expand fails the build, and so does any relative
`src`, `href` or `srcset` that names a file the build did not write.

## Deployment

Pages is configured to deploy from the workflow. The `build` job renders and
verifies the site on pushes and pull requests; the `deploy` job runs only for a
push to `main`, in the `github-pages` environment, and never cancels a
deployment already under way.
