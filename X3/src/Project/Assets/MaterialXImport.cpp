// =============================================================================
// MaterialXImport.cpp -- the .mtlx reader and the OpenPBR -> MaterialDesc
// reduction. The mapping table, the drop list and the dependency argument all
// live in MaterialXImport.h; this file is the mechanism.
//
// Two layers, deliberately separate:
//   1. A tiny XML reader (anonymous namespace, below). Knows nothing about
//      MaterialX.
//   2. The MaterialX/OpenPBR layer, which never touches raw text.
//
// Keeping them apart is what makes the reduction reviewable: every decision
// about a lobe is in one function with a comment, not tangled with quote
// handling.
// =============================================================================

#include "lrpch.h"

#include "Project/Assets/MaterialXImport.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace X3
{
	namespace {

		// =====================================================================
		// LAYER 1 -- the XML reader.
		//
		// WHAT IT SUPPORTS: elements, attributes (single- or double-quoted),
		// self-closing tags, nesting, comments, the <?xml?> prolog, processing
		// instructions, a naive <!DOCTYPE ...> skip, CDATA sections, and the
		// five predefined entities plus numeric character references.
		//
		// WHAT IT DOES NOT SUPPORT, and why that is fine for .mtlx:
		//   - Namespaces. MaterialX uses none; a prefix would just become part
		//     of the element name.
		//   - Custom DTD entities. MaterialX has no DTD.
		//   - Element TEXT CONTENT. It is parsed past and discarded, because
		//     MaterialX carries every value in an attribute. If that ever stops
		//     being true this reader will silently return nothing for the
		//     affected input rather than a wrong value -- which is the failure
		//     mode we want.
		//   - Encodings other than UTF-8/ASCII. UTF-8 passes through byte-wise;
		//     a UTF-16 document is rejected at the first byte rather than being
		//     read as garbage.
		//
		// It is a RECOGNISER FOR WELL-FORMED INPUT, not a validator. It rejects
		// what it cannot understand instead of guessing, because a bake tool
		// that guesses produces a plausible wrong material.
		// =====================================================================

		// Bounds the recursion in both the reader and the node-chain walker. A
		// hand-edited or generated document with a cycle -- or 10,000 nested
		// elements -- would otherwise take the stack out, and a crash in a bake
		// tool is indistinguishable from a build system bug.
		constexpr int kMaxXmlDepth   = 64;
		constexpr int kMaxChainDepth = 16;

		struct XmlNode {
			std::string name;
			std::vector<std::pair<std::string, std::string>> attrs;
			std::vector<XmlNode> children;

			const std::string* Attr(std::string_view key) const {
				for (const auto& a : attrs)
					if (a.first == key) return &a.second;
				return nullptr;
			}
			std::string_view AttrOr(std::string_view key, std::string_view fallback = {}) const {
				const std::string* v = Attr(key);
				return v ? std::string_view(*v) : fallback;
			}
		};

		class XmlReader {
		public:
			explicit XmlReader(std::string_view src) : m_Src(src) {}

			bool Parse(XmlNode& root) {
				SkipMisc();
				if (!ParseElement(root, 0)) return false;
				return true;
			}

			const std::string& Error() const { return m_Error; }

		private:
			std::string_view m_Src;
			size_t           m_Pos = 0;
			std::string      m_Error;

			bool Eof() const { return m_Pos >= m_Src.size(); }
			char Peek(size_t ahead = 0) const {
				return (m_Pos + ahead < m_Src.size()) ? m_Src[m_Pos + ahead] : '\0';
			}
			/// Guarded against m_Pos == size(): string_view::compare THROWS on an
			/// out-of-range pos, and an exception escaping a bake tool at EOF is
			/// a crash with no message rather than a parse error with a line.
			bool Starts(std::string_view s) const {
				if (m_Pos >= m_Src.size()) return false;
				return m_Src.compare(m_Pos, s.size(), s) == 0;
			}

			/// Errors carry a line number. A parse failure reported as "malformed
			/// document" against a 4000-line Substance export is not actionable.
			bool Fail(std::string_view what) {
				if (!m_Error.empty()) return false;   // keep the first, deepest cause
				size_t line = 1;
				for (size_t i = 0; i < m_Pos && i < m_Src.size(); ++i)
					if (m_Src[i] == '\n') ++line;
				m_Error = "line " + std::to_string(line) + ": " + std::string(what);
				return false;
			}

			static bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
			static bool IsNameEnd(char c) {
				return IsSpace(c) || c == '/' || c == '>' || c == '=' || c == '\0';
			}

			void SkipSpace() { while (!Eof() && IsSpace(m_Src[m_Pos])) ++m_Pos; }

			/// Whitespace, comments, processing instructions and DOCTYPE, in any
			/// order and any number -- which is what "Misc" means in the XML
			/// grammar and what actually appears above a <materialx> root.
			void SkipMisc() {
				for (;;) {
					SkipSpace();
					if (Starts("<!--")) { SkipTo("-->", 3); continue; }
					if (Starts("<?"))   { SkipTo("?>",  2); continue; }
					// Naive: stops at the first '>', so an internal DTD subset
					// containing '>' would break. MaterialX has no DTD, and
					// getting this wrong on a document that does is a clean
					// parse error rather than a wrong value.
					if (Starts("<!"))   { SkipTo(">",   1); continue; }
					return;
				}
			}

			void SkipTo(std::string_view terminator, size_t termLen) {
				const size_t at = m_Src.find(terminator, m_Pos);
				m_Pos = (at == std::string_view::npos) ? m_Src.size() : at + termLen;
			}

			/// The five predefined entities plus numeric references. Needed
			/// because a texture path may legitimately contain '&' and a
			/// document that escapes it must not produce a path with a literal
			/// "&amp;" in it -- which resolves to nothing, at bake time, with no
			/// obvious cause.
			static void AppendDecoded(std::string& out, std::string_view raw) {
				for (size_t i = 0; i < raw.size(); ) {
					if (raw[i] != '&') { out.push_back(raw[i++]); continue; }
					const size_t end = raw.find(';', i);
					if (end == std::string_view::npos) { out.push_back(raw[i++]); continue; }

					const std::string_view ent = raw.substr(i + 1, end - i - 1);
					if      (ent == "amp")  out.push_back('&');
					else if (ent == "lt")   out.push_back('<');
					else if (ent == "gt")   out.push_back('>');
					else if (ent == "quot") out.push_back('"');
					else if (ent == "apos") out.push_back('\'');
					else if (!ent.empty() && ent[0] == '#') {
						unsigned long code = 0;
						const bool hex = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X');
						const std::string digits(ent.substr(hex ? 2 : 1));
						code = std::strtoul(digits.c_str(), nullptr, hex ? 16 : 10);
						// ASCII only. A non-ASCII reference is passed through
						// verbatim rather than mis-encoded; MaterialX documents
						// that need one are already UTF-8 in the raw bytes.
						if (code > 0 && code < 128) out.push_back(static_cast<char>(code));
						else out.append(raw.substr(i, end - i + 1));
					}
					else out.append(raw.substr(i, end - i + 1));   // unknown entity: leave it alone
					i = end + 1;
				}
			}

			bool ParseElement(XmlNode& node, int depth) {
				if (depth > kMaxXmlDepth) return Fail("element nesting deeper than the reader's limit");
				if (Peek() != '<') return Fail("expected '<'");
				++m_Pos;

				const size_t nameStart = m_Pos;
				while (!Eof() && !IsNameEnd(m_Src[m_Pos])) ++m_Pos;
				if (m_Pos == nameStart) return Fail("empty element name");
				node.name.assign(m_Src.substr(nameStart, m_Pos - nameStart));

				for (;;) {
					SkipSpace();
					if (Eof()) return Fail("unterminated start tag");

					if (Starts("/>")) { m_Pos += 2; return true; }        // no children
					if (Peek() == '>') { ++m_Pos; break; }                // children follow

					const size_t attrStart = m_Pos;
					while (!Eof() && !IsNameEnd(m_Src[m_Pos])) ++m_Pos;
					if (m_Pos == attrStart) return Fail("expected an attribute name");
					std::string key(m_Src.substr(attrStart, m_Pos - attrStart));

					SkipSpace();
					if (Peek() != '=') return Fail("attribute '" + key + "' has no value");
					++m_Pos;
					SkipSpace();

					const char quote = Peek();
					if (quote != '"' && quote != '\'') return Fail("attribute '" + key + "' value is not quoted");
					++m_Pos;
					const size_t valStart = m_Pos;
					while (!Eof() && m_Src[m_Pos] != quote) ++m_Pos;
					if (Eof()) return Fail("unterminated attribute value");

					std::string value;
					AppendDecoded(value, m_Src.substr(valStart, m_Pos - valStart));
					++m_Pos;   // closing quote

					node.attrs.emplace_back(std::move(key), std::move(value));
				}

				// --- children -------------------------------------------------
				for (;;) {
					// Text content is skipped, not stored. See the header note.
					while (!Eof() && m_Src[m_Pos] != '<') ++m_Pos;
					if (Eof()) return Fail("unterminated element <" + node.name + ">");

					if (Starts("<!--"))      { SkipTo("-->",  3); continue; }
					if (Starts("<![CDATA[")) { SkipTo("]]>",  3); continue; }
					if (Starts("<?"))        { SkipTo("?>",   2); continue; }

					if (Starts("</")) {
						m_Pos += 2;
						const size_t closeStart = m_Pos;
						while (!Eof() && !IsNameEnd(m_Src[m_Pos])) ++m_Pos;
						const std::string_view closeName = m_Src.substr(closeStart, m_Pos - closeStart);
						if (closeName != node.name)
							return Fail("</" + std::string(closeName) + "> closes <" + node.name + ">");
						SkipSpace();
						if (Peek() != '>') return Fail("malformed end tag for <" + node.name + ">");
						++m_Pos;
						return true;
					}

					node.children.emplace_back();
					if (!ParseElement(node.children.back(), depth + 1)) return false;
				}
			}
		};

		// =====================================================================
		// LAYER 2 -- MaterialX.
		// =====================================================================

		/// std::from_chars rather than strtof/atof: it is LOCALE-INDEPENDENT by
		/// definition. strtof honours LC_NUMERIC, and a process that has called
		/// setlocale for a German locale parses "0.5" as 0. That failure is
		/// machine-dependent, silent, and produces materials that differ between
		/// two artists' workstations.
		bool ParseFloat(std::string_view token, float& out) {
			while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front()))) token.remove_prefix(1);
			while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back())))  token.remove_suffix(1);
			if (token.empty()) return false;
			if (token == "true")  { out = 1.0f; return true; }
			if (token == "false") { out = 0.0f; return true; }

			// from_chars deliberately rejects a leading '+'; some exporters write
			// one. Dropping it here is cheaper than the alternative, which is the
			// whole input reading as unset.
			if (token.front() == '+') token.remove_prefix(1);
			if (token.empty()) return false;

			float v = 0.0f;
			const auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), v);
			if (ec != std::errc() || ptr != token.data() + token.size()) return false;
			out = v;
			return true;
		}

		/// MaterialX vector literals are comma-separated; whitespace around the
		/// commas is optional and inconsistent across exporters.
		int ParseFloatList(std::string_view s, float* out, int maxCount) {
			int n = 0;
			size_t i = 0;
			while (i <= s.size() && n < maxCount) {
				size_t comma = s.find(',', i);
				if (comma == std::string_view::npos) comma = s.size();
				if (!ParseFloat(s.substr(i, comma - i), out[n])) return n;
				++n;
				if (comma == s.size()) break;
				i = comma + 1;
			}
			return n;
		}

		/// Which MaterialX colour spaces are non-linearly encoded. `g22_*` is
		/// gamma 2.2, approximated here by the sRGB EOTF -- under 1% error, and
		/// treating it as linear instead would be a visible ~2x on midtones.
		bool IsSrgbEncoded(std::string_view cs) {
			return cs == "srgb_texture" || cs == "sRGB" || cs == "srgb"
			    || cs == "g22_rec709"   || cs == "g22_ap1";
		}
		bool IsColorSpaceKnown(std::string_view cs) {
			return cs.empty() || IsSrgbEncoded(cs)
			    || cs == "lin_rec709" || cs == "linear" || cs == "acescg"
			    || cs == "lin_ap1"    || cs == "none"   || cs == "raw";
		}

		float SrgbToLinear(float c) {
			return (c <= 0.04045f) ? (c / 12.92f) : std::pow((c + 0.055f) / 1.055f, 2.4f);
		}

		// --- node lookup -----------------------------------------------------
		//
		// MaterialX node names are unique WITHIN THEIR SCOPE -- the document
		// root, or one nodegraph -- and not globally. Two nodegraphs both
		// containing an "image1" is normal output from every DCC that writes
		// mtlx, so a single flat map would resolve half the references to the
		// wrong image and produce a material textured with someone else's maps.
		struct Scope {
			const XmlNode* graph = nullptr;   ///< the <nodegraph>, or null for the document root
			std::unordered_map<std::string, const XmlNode*> nodes;
		};

		struct Doc {
			const XmlNode* root = nullptr;
			Scope rootScope;
			std::unordered_map<const XmlNode*, Scope> graphScopes;   ///< keyed by the <nodegraph> element

			const Scope& ScopeFor(const XmlNode* graph) const {
				if (graph) {
					const auto it = graphScopes.find(graph);
					if (it != graphScopes.end()) return it->second;
				}
				return rootScope;
			}
			const XmlNode* Find(const XmlNode* graph, const std::string& name) const {
				const Scope& s = ScopeFor(graph);
				const auto it = s.nodes.find(name);
				return (it == s.nodes.end()) ? nullptr : it->second;
			}
		};

		void BuildScopes(Doc& doc) {
			for (const XmlNode& child : doc.root->children) {
				// <nodedef> and <implementation> DECLARE nodes, they do not
				// instantiate them. Indexing their contents would make the
				// importer pick up the OpenPBR nodedef shipped in a library
				// include and import the specification's defaults as if they
				// were a material in the document.
				if (child.name == "nodedef" || child.name == "implementation") continue;

				if (const std::string* n = child.Attr("name"))
					doc.rootScope.nodes[*n] = &child;

				if (child.name == "nodegraph") {
					Scope s;
					s.graph = &child;
					for (const XmlNode& inner : child.children)
						if (const std::string* n = inner.Attr("name"))
							s.nodes[*n] = &inner;
					doc.graphScopes[&child] = std::move(s);
				}
			}
		}

		/// An <input> (1.38+) or <parameter> (1.37, merged into input in 1.38).
		/// Accepting both costs four characters and stops a 1.37 document from
		/// parsing cleanly while reading as if every value were unset.
		const XmlNode* FindInput(const XmlNode& node, std::string_view name) {
			for (const XmlNode& c : node.children)
				if ((c.name == "input" || c.name == "parameter") && c.AttrOr("name") == name)
					return &c;
			return nullptr;
		}

		bool IsInputElement(const XmlNode& n) { return n.name == "input" || n.name == "parameter"; }

		/// True when this input names an upstream node instead of carrying a value.
		bool IsReference(const XmlNode& input) {
			return input.Attr("nodename") || input.Attr("nodegraph") || input.Attr("interfacename");
		}

		// --- evaluation ------------------------------------------------------

		enum class ValueKind { None, Literal, Texture, Graph };

		struct Eval {
			ValueKind kind = ValueKind::None;
			float     v[4] = { 0, 0, 0, 0 };
			int       count = 0;

			std::string file;          ///< set when kind == Texture
			std::string colorSpace;    ///< as declared on the file input or the image node

			bool  hasNormalScale = false;   ///< a <normalmap> was crossed on the way to the image
			float normalScale    = 1.0f;

			std::string graphCategory;  ///< set when kind == Graph: what we refused to evaluate
		};

		struct Resolved {
			const XmlNode* node       = nullptr;   ///< upstream node
			const XmlNode* valueInput = nullptr;   ///< or an element carrying a literal (a nodegraph interface default)
			const XmlNode* scope      = nullptr;   ///< the nodegraph containing `node`, null for the document root
		};

		/// nodename= / nodegraph=+output= / interfacename=, which is the complete
		/// set of ways one MaterialX element points at another.
		Resolved ResolveReference(const Doc& doc, const XmlNode* scope, const XmlNode& input,
		                          std::vector<std::string>& warnings) {
			Resolved r;
			r.scope = scope;

			if (const std::string* nodegraphName = input.Attr("nodegraph")) {
				const XmlNode* graph = doc.Find(nullptr, *nodegraphName);
				if (!graph || graph->name != "nodegraph") {
					warnings.push_back("input '" + std::string(input.AttrOr("name")) +
					                   "' names nodegraph '" + *nodegraphName + "', which does not exist");
					return r;
				}
				// The named output, or the only one. A graph with several
				// outputs and no output= is ambiguous; picking the first would
				// silently texture the wrong channel.
				const XmlNode* chosen = nullptr;
				int outputCount = 0;
				for (const XmlNode& c : graph->children) {
					if (c.name != "output") continue;
					++outputCount;
					if (const std::string* want = input.Attr("output")) {
						if (c.AttrOr("name") == *want) chosen = &c;
					}
					else if (!chosen) chosen = &c;
				}
				if (!chosen || (outputCount > 1 && !input.Attr("output"))) {
					warnings.push_back("nodegraph '" + *nodegraphName + "' has " +
					                   std::to_string(outputCount) + " outputs and the reference does not say which");
					return r;
				}
				r.scope = graph;
				if (const std::string* upstream = chosen->Attr("nodename"))
					r.node = doc.Find(graph, *upstream);
				else
					r.valueInput = chosen;   // an output carrying a constant
				return r;
			}

			if (const std::string* interfaceName = input.Attr("interfacename")) {
				// Inside a nodegraph, pointing at the graph's own published
				// input. Without this, every published-parameter document (which
				// is what "make this tweakable in the DCC" produces) reads as
				// unset.
				const XmlNode* graph = scope;
				if (!graph) {
					warnings.push_back("interfacename '" + *interfaceName + "' used outside a nodegraph");
					return r;
				}
				if (const XmlNode* gi = FindInput(*graph, *interfaceName)) r.valueInput = gi;
				return r;
			}

			if (const std::string* nodeName = input.Attr("nodename")) {
				r.node = doc.Find(scope, *nodeName);
				if (!r.node)
					warnings.push_back("input '" + std::string(input.AttrOr("name")) +
					                   "' names node '" + *nodeName + "', which does not exist in its scope");
			}
			return r;
		}

		Eval EvaluateInput(const Doc& doc, const XmlNode* scope, const XmlNode& input,
		                   int depth, std::vector<std::string>& warnings);

		/// Walk upstream from a node looking for something we can bake: an image
		/// file, or a constant. Everything else is reported, never guessed at.
		Eval EvaluateNode(const Doc& doc, const XmlNode* scope, const XmlNode& node,
		                  int depth, std::vector<std::string>& warnings) {
			Eval e;
			if (depth > kMaxChainDepth) {
				// A cycle, or a chain so deep it is not a material any more.
				e.kind = ValueKind::Graph;
				e.graphCategory = node.name + " (chain too deep -- possible cycle)";
				return e;
			}

			if (node.name == "image" || node.name == "tiledimage") {
				const XmlNode* fileInput = FindInput(node, "file");
				if (fileInput) {
					if (const std::string* f = fileInput->Attr("value")) {
						e.kind = ValueKind::Texture;
						e.file = *f;
						// The colorspace may sit on the file input or on the
						// image node. Reading only one of the two is how a
						// normal map ends up decoded through an EOTF.
						std::string_view cs = fileInput->AttrOr("colorspace");
						if (cs.empty()) cs = node.AttrOr("colorspace");
						e.colorSpace.assign(cs);
						return e;
					}
					// A file input driven by a reference (a switch between
					// texture variants) is a graph, not a file.
				}
				e.kind = ValueKind::Graph;
				e.graphCategory = node.name + " (no literal file)";
				return e;
			}

			if (node.name == "constant") {
				if (const XmlNode* v = FindInput(node, "value"))
					return EvaluateInput(doc, scope, *v, depth + 1, warnings);
			}

			// A normalmap wraps the image and carries the scale MaterialDesc
			// wants. Capture it on the way past; the image is further up.
			float capturedScale = 1.0f;
			bool  haveScale = false;
			if (node.name == "normalmap") {
				if (const XmlNode* s = FindInput(node, "scale")) {
					if (const std::string* raw = s->Attr("value")) {
						float f = 1.0f;
						if (ParseFloat(*raw, f)) { capturedScale = f; haveScale = true; }
					}
				}
			}

			for (const XmlNode& c : node.children) {
				if (!IsInputElement(c) || !IsReference(c)) continue;
				Eval up = EvaluateInput(doc, scope, c, depth + 1, warnings);
				if (up.kind == ValueKind::Texture || up.kind == ValueKind::Literal) {
					if (haveScale && !up.hasNormalScale) { up.hasNormalScale = true; up.normalScale = capturedScale; }
					return up;
				}
			}

			e.kind = ValueKind::Graph;
			e.graphCategory = node.name;
			return e;
		}

		Eval EvaluateInput(const Doc& doc, const XmlNode* scope, const XmlNode& input,
		                   int depth, std::vector<std::string>& warnings) {
			Eval e;
			if (depth > kMaxChainDepth) {
				e.kind = ValueKind::Graph;
				e.graphCategory = "input chain too deep -- possible cycle";
				return e;
			}

			if (const std::string* raw = input.Attr("value")) {
				e.count = ParseFloatList(*raw, e.v, 4);
				if (e.count == 0) {
					// A filename or a string enum, not a number. Callers that
					// wanted a scalar treat None as "the document was silent",
					// which is the correct outcome.
					return e;
				}
				e.kind = ValueKind::Literal;

				// A COLOUR LITERAL CAN CARRY ITS OWN ENCODING. Substance writes
				// base_color as an srgb_texture-space triple often enough that
				// ignoring the attribute leaves every imported albedo visibly
				// too bright.
				const std::string_view cs = input.AttrOr("colorspace");
				const std::string_view type = input.AttrOr("type");
				if (IsSrgbEncoded(cs) && (type == "color3" || type == "color4"))
					for (int i = 0; i < std::min(e.count, 3); ++i)
						e.v[i] = SrgbToLinear(e.v[i]);
				else if (!IsColorSpaceKnown(cs))
					warnings.push_back("input '" + std::string(input.AttrOr("name")) +
					                   "' declares unknown colorspace '" + std::string(cs) +
					                   "'; treated as linear");
				return e;
			}

			if (!IsReference(input)) return e;   // no value, no reference: nothing to read

			const Resolved r = ResolveReference(doc, scope, input, warnings);
			if (r.valueInput && r.valueInput != &input)
				return EvaluateInput(doc, r.scope, *r.valueInput, depth + 1, warnings);
			if (r.node)
				return EvaluateNode(doc, r.scope, *r.node, depth + 1, warnings);
			return e;
		}

		// --- the reduction ---------------------------------------------------

		/// Look up the first input matching any of the accepted spellings, and
		/// report WHICH one matched -- the caller needs that, because
		/// standard_surface's `emission` is a unitless weight while OpenPBR's
		/// `emission_luminance` is in nits, and the two must not be treated
		/// alike.
		struct Match {
			const XmlNode* input = nullptr;
			std::string    name;
			explicit operator bool() const { return input != nullptr; }
		};

		Match FindAliased(const XmlNode& shader, std::initializer_list<const char*> names) {
			for (const char* n : names)
				if (const XmlNode* in = FindInput(shader, n)) return { in, n };
			return {};
		}

		/// Inputs we deliberately drop, and the reason to print. Anything the
		/// document sets that is not consumed and not in here is reported as
		/// unrecognised -- so a future OpenPBR revision adding a channel shows up
		/// in the bake log instead of vanishing.
		struct DropReason { const char* input; const char* why; };
		constexpr DropReason kDropped[] = {
			{ "transmission_weight",              "there is no MaterialDesc field -- transmission needs a refraction path (Phase 7 pass 4)" },
			{ "transmission",                     "there is no MaterialDesc field -- transmission needs a refraction path (Phase 7 pass 4)" },
			{ "transmission_color",               "no MaterialDesc field for transmission" },
			{ "transmission_depth",               "no MaterialDesc field for transmission" },
			{ "transmission_scatter",             "no MaterialDesc field for transmission" },
			{ "transmission_scatter_anisotropy",  "no MaterialDesc field for transmission" },
			{ "transmission_dispersion_scale",    "no MaterialDesc field for transmission" },
			{ "transmission_dispersion_abbe_number", "no MaterialDesc field for transmission" },
			{ "transmission_extra_roughness",     "no MaterialDesc field for transmission" },
			{ "subsurface_weight",                "there is no MaterialDesc field -- the BSDF library has no subsurface lobe" },
			{ "subsurface",                       "there is no MaterialDesc field -- the BSDF library has no subsurface lobe" },
			{ "subsurface_color",                 "no subsurface lobe" },
			{ "subsurface_radius",                "no subsurface lobe" },
			{ "subsurface_radius_scale",          "no subsurface lobe" },
			{ "subsurface_scale",                 "no subsurface lobe" },
			{ "subsurface_scatter_anisotropy",    "no subsurface lobe" },
			{ "subsurface_anisotropy",            "no subsurface lobe" },
			{ "thin_film_weight",                 "Gpu::MaterialExt has thin-film slots but MaterialDesc does not expose them" },
			{ "thin_film_thickness",              "Gpu::MaterialExt has thin-film slots but MaterialDesc does not expose them" },
			{ "thin_film_ior",                    "Gpu::MaterialExt has thin-film slots but MaterialDesc does not expose them" },
			{ "thin_film_IOR",                    "Gpu::MaterialExt has thin-film slots but MaterialDesc does not expose them" },
			{ "coat_color",                       "no coat tint in MaterialDesc; the coat is colourless" },
			{ "coat_ior",                         "Gpu::MaterialExt fixes the coat IOR at 1.5" },
			{ "coat_IOR",                         "Gpu::MaterialExt fixes the coat IOR at 1.5" },
			{ "coat_darkening",                   "not modelled" },
			{ "coat_affect_color",                "not modelled" },
			{ "coat_affect_roughness",            "not modelled" },
			{ "coat_roughness_anisotropy",        "the coat lobe is isotropic" },
			{ "coat_anisotropy",                  "the coat lobe is isotropic" },
			{ "coat_rotation",                    "the coat lobe is isotropic" },
			{ "coat_normal",                      "one normal slot only; a second normal set needs a second UV chain" },
			{ "geometry_coat_normal",             "one normal slot only; a second normal set needs a second UV chain" },
			{ "geometry_tangent",                 "MaterialDesc::anisotropy is relative to the MESH tangent; a tangent map is not reachable" },
			{ "geometry_coat_tangent",            "one tangent only" },
			{ "tangent",                          "MaterialDesc::anisotropy is relative to the MESH tangent" },
			{ "base_diffuse_roughness",           "the diffuse lobe is Lambert; Oren-Nayar is not implemented" },
			{ "diffuse_roughness",                "the diffuse lobe is Lambert; Oren-Nayar is not implemented" },
			{ "geometry_thin_walled",             "no thin-walled path in the BSDF" },
			{ "thin_walled",                      "no thin-walled path in the BSDF" },
			{ "specular_color",                   "no specular tint in MaterialDesc; Gpu::MaterialExt's tint is not exposed" },
			{ "specular_rotation",                "MaterialDesc has no anisotropy rotation" },
			{ "specular_roughness_anisotropy_rotation", "MaterialDesc has no anisotropy rotation" },
			{ "emission_mode",                    "not modelled" },
			{ "geometry_normal_scale",            "folded into normalScale via the normalmap node instead" },
		};

		const char* DropReasonFor(std::string_view input) {
			for (const DropReason& d : kDropped)
				if (input == d.input) return d.why;
			return nullptr;
		}

		std::string Fmt(float v) {
			char buf[32];
			std::snprintf(buf, sizeof(buf), "%.4g", static_cast<double>(v));
			return buf;
		}

		// One material's worth of state, so the texture bookkeeping does not
		// have to be threaded through a dozen parameters.
		struct Reducer {
			const Doc&               doc;
			const MtlxImportOptions& opt;
			MtlxMaterial&            out;
			std::vector<std::string>& warnings;
			std::unordered_set<std::string> consumed;

			Eval Read(const XmlNode& shader, std::initializer_list<const char*> names, Match& matched) {
				matched = FindAliased(shader, names);
				if (!matched) return {};

				// Consume EVERY spelling of this channel the document sets, not
				// just the one we used. Otherwise a document carrying both
				// `base_metalness` and `metalness` reports the loser as
				// "not recognised by this importer", which sends whoever reads
				// the bake log hunting for a parser bug that is not there.
				for (const char* n : names) {
					if (!FindInput(shader, n)) continue;
					consumed.insert(n);
					if (matched.name != n)
						Note("'" + std::string(n) + "' is also set and was ignored; '" +
						     matched.name + "' took precedence");
				}
				return EvaluateInput(doc, nullptr, *matched.input, 0, warnings);
			}

			void Note(std::string line) { out.reductions.push_back(std::move(line)); }

			/// Record a texture and, when MaterialDesc has a slot for it, resolve
			/// it to a GUID. Colour space defaults PER SLOT because it is not
			/// cosmetic: a normal or ORM map sampled through an sRGB view has an
			/// EOTF applied to numbers that are not light, and it reads as a
			/// shading bug rather than a format bug.
			void RecordTexture(std::string_view input, const Eval& e, bool defaultSRGB, LR_GUID* slot) {
				MtlxTextureRef ref;
				ref.input.assign(input);
				ref.file       = e.file;
				ref.colorSpace = e.colorSpace;
				ref.isSRGB     = e.colorSpace.empty() ? defaultSRGB : IsSrgbEncoded(e.colorSpace);
				ref.used       = (slot != nullptr);

				std::filesystem::path p(e.file);
				ref.resolved = p.is_relative() && !opt.documentDir.empty()
				             ? (opt.documentDir / p).lexically_normal()
				             : p;

				if (!e.colorSpace.empty() && !IsColorSpaceKnown(e.colorSpace))
					warnings.push_back("texture on '" + std::string(input) + "' declares unknown colorspace '" +
					                   e.colorSpace + "'; treated as " + (defaultSRGB ? "sRGB" : "linear"));

				if (slot && opt.resolveTexture) *slot = opt.resolveTexture(ref.resolved, ref.isSRGB);
				if (!slot)
					Note("DROPPED texture on '" + std::string(input) + "' (" + e.file +
					     "): not imported into MaterialDesc");

				out.textures.push_back(std::move(ref));
			}
		};

		/// MaterialDesc::specularLevel is an F0 SCALE where 0.5 means the usual
		/// 0.04 -- i.e. F0 = 0.08 * level, the Autodesk/glTF convention. Going
		/// from an IOR is the Schlick inversion.
		///
		/// It CLAMPS: level 1 is F0 0.08, which is ior ~1.79. Diamond (2.42, F0
		/// 0.17) and heavy flint glass cannot be represented, and clamping is
		/// the honest failure -- letting the level exceed 1 would push F0 past
		/// what the shader's dielectric term expects and make grazing angles go
		/// wrong instead of just the normal-incidence reflectance.
		float IorToSpecularLevel(float ior) {
			if (!(ior > 1.0f)) return 0.0f;         // ior <= 1 (or NaN): no dielectric reflection
			const float f0 = (ior - 1.0f) / (ior + 1.0f);
			return std::min(f0 * f0 / 0.08f, 1.0f);
		}

		void ReduceShader(const Doc& doc, const XmlNode& shader, const MtlxImportOptions& opt,
		                  MtlxMaterial& out, std::vector<std::string>& warnings) {
			Reducer R{ doc, opt, out, warnings, {} };
			MaterialDesc& d = out.desc;
			Match m;

			// --- BASE -------------------------------------------------------
			// base_weight is FOLDED INTO THE ALBEDO. OpenPBR weights the diffuse
			// lobe; scaling base_color reproduces that exactly for the diffuse
			// term and gets the specular-over-diffuse energy split wrong. There
			// is no MaterialDesc field to do better with.
			float baseWeight = 1.0f;
			Eval e = R.Read(shader, { "base_weight", "base" }, m);
			if (e.kind == ValueKind::Literal) {
				baseWeight = e.v[0];
				if (baseWeight != 1.0f)
					R.Note("APPROXIMATED " + m.name + " " + Fmt(baseWeight) +
					       " -> folded into color.rgb (OpenPBR weights the diffuse lobe, we scale albedo)");
			}
			else if (e.kind == ValueKind::Graph) {
				R.Note("DROPPED " + m.name + ": driven by an unevaluated " + e.graphCategory + " graph");
			}

			e = R.Read(shader, { "base_color" }, m);
			if (e.kind == ValueKind::Literal && e.count >= 3) {
				d.color = { e.v[0] * baseWeight, e.v[1] * baseWeight, e.v[2] * baseWeight, d.color.a };
			}
			else if (e.kind == ValueKind::Texture) {
				R.RecordTexture("base_color", e, /*defaultSRGB*/ true, &d.baseColorTex);
				if (baseWeight != 1.0f)
					d.color = { baseWeight, baseWeight, baseWeight, d.color.a };
			}
			else if (e.kind == ValueKind::Graph) {
				R.Note("DROPPED base_color: driven by an unevaluated " + e.graphCategory + " graph");
			}
			else if (baseWeight != 1.0f) {
				d.color = { baseWeight, baseWeight, baseWeight, d.color.a };
			}

			// --- METALNESS / ROUGHNESS -------------------------------------
			// Held aside rather than assigned immediately: MaterialDesc has ONE
			// ORM slot and the decision needs both.
			Eval metalTex, roughTex;

			e = R.Read(shader, { "base_metalness", "metalness" }, m);
			if (e.kind == ValueKind::Literal)      d.metallic = e.v[0];
			else if (e.kind == ValueKind::Texture) metalTex = e;
			else if (e.kind == ValueKind::Graph)   R.Note("DROPPED " + m.name + ": unevaluated " + e.graphCategory + " graph");

			e = R.Read(shader, { "specular_roughness" }, m);
			if (e.kind == ValueKind::Literal)      d.roughness = e.v[0];
			else if (e.kind == ValueKind::Texture) roughTex = e;
			else if (e.kind == ValueKind::Graph)   R.Note("DROPPED specular_roughness: unevaluated " + e.graphCategory + " graph");

			if (roughTex.kind == ValueKind::Texture && metalTex.kind == ValueKind::Texture &&
			    roughTex.file != metalTex.file) {
				// glTF's ORM packing is the only thing MaterialDesc can express.
				// Two separate files cannot both be honoured; taking roughness
				// keeps the more visually significant channel and the drop is
				// reported rather than silent.
				R.RecordTexture("specular_roughness", roughTex, false, &d.metalRoughTex);
				R.RecordTexture("base_metalness", metalTex, false, nullptr);
				R.Note("APPROXIMATED metalness and roughness are separate images (" + metalTex.file +
				       ", " + roughTex.file + "); MaterialDesc has one ORM slot, so roughness was taken "
				       "and the metalness image was dropped");
			}
			else if (roughTex.kind == ValueKind::Texture) {
				R.RecordTexture("specular_roughness", roughTex, false, &d.metalRoughTex);
				if (metalTex.kind == ValueKind::Texture)
					R.Note("metalness and roughness share " + roughTex.file +
					       ", read as a glTF-packed ORM (G=roughness, B=metalness)");
			}
			else if (metalTex.kind == ValueKind::Texture) {
				R.RecordTexture("base_metalness", metalTex, false, &d.metalRoughTex);
			}

			// --- SPECULAR ---------------------------------------------------
			// Three spellings reach the same field. specular_ior_level is the
			// draft name and is already the 0..1 level; specular_ior is the v1.x
			// name and needs the Schlick inversion.
			bool haveLevel = false;
			e = R.Read(shader, { "specular_ior_level", "specular_level" }, m);
			if (e.kind == ValueKind::Literal) { d.specularLevel = e.v[0]; haveLevel = true; }

			// Read the IOR group UNCONDITIONALLY even when a level already won,
			// so that it is consumed and reported as superseded rather than
			// falling through to the "not recognised" sweep at the bottom.
			Eval ior = R.Read(shader, { "specular_ior", "specular_IOR" }, m);
			if (ior.kind == ValueKind::Literal) {
				if (haveLevel) {
					R.Note("'" + m.name + "' ignored: a specular level was set as well and is used directly");
				}
				else {
					const float level = IorToSpecularLevel(ior.v[0]);
					d.specularLevel = level;
					haveLevel = true;
					if (level >= 1.0f)
						R.Note("APPROXIMATED " + m.name + " " + Fmt(ior.v[0]) +
						       " clamps specularLevel to 1 (F0 0.08, ior ~1.79); the extra reflectance is lost");
					else
						R.Note("APPROXIMATED " + m.name + " " + Fmt(ior.v[0]) + " -> specularLevel " + Fmt(level) +
						       " via F0 = ((ior-1)/(ior+1))^2 / 0.08");
				}
			}

			e = R.Read(shader, { "specular_weight", "specular" }, m);
			if (e.kind == ValueKind::Literal && e.v[0] != 1.0f) {
				// The weight scales the whole specular lobe in OpenPBR; here it
				// only scales F0, so grazing-angle Fresnel still climbs to 1.
				// That is the error the Adobe-reference comparison would measure.
				d.specularLevel *= e.v[0];
				haveLevel = true;
				R.Note("APPROXIMATED " + m.name + " " + Fmt(e.v[0]) +
				       " folded into specularLevel; it scales F0 only, not the whole lobe");
			}
			(void)haveLevel;

			e = R.Read(shader, { "specular_roughness_anisotropy", "specular_anisotropy" }, m);
			if (e.kind == ValueKind::Literal) {
				d.anisotropy = e.v[0];
				R.Note("APPROXIMATED " + m.name + ": magnitude kept, tangent rotation dropped "
				       "(MaterialDesc::anisotropy is signed and relative to the mesh tangent)");
			}

			// --- COAT -------------------------------------------------------
			e = R.Read(shader, { "coat_weight", "coat" }, m);
			if (e.kind == ValueKind::Literal) d.clearcoat = e.v[0];
			else if (e.kind == ValueKind::Texture)
				R.RecordTexture(m.name, e, false, nullptr);   // no coat texture slot

			e = R.Read(shader, { "coat_roughness" }, m);
			if (e.kind == ValueKind::Literal) d.clearcoatRough = e.v[0];
			else if (e.kind == ValueKind::Texture)
				R.RecordTexture("coat_roughness", e, false, nullptr);

			// --- FUZZ / SHEEN ------------------------------------------------
			// OpenPBR v1.x renamed sheen to fuzz. The weight is FOLDED INTO THE
			// COLOUR, which is exact rather than approximate for this struct:
			// hasExtendedLobes() tests the colour, so a weight of zero produces
			// black and costs nothing -- which is precisely the intent recorded
			// in MaterialDesc.h.
			Eval fuzzColor = R.Read(shader, { "fuzz_color", "sheen_color" }, m);
			const std::string fuzzColorName = m ? m.name : std::string();
			Eval fuzzWeight = R.Read(shader, { "fuzz_weight", "sheen_weight", "sheen" }, m);
			const std::string fuzzWeightName = m ? m.name : std::string();

			if (fuzzWeight.kind == ValueKind::Literal) {
				glm::vec3 c(1.0f);
				if (fuzzColor.kind == ValueKind::Literal && fuzzColor.count >= 3)
					c = { fuzzColor.v[0], fuzzColor.v[1], fuzzColor.v[2] };
				d.sheenColor = c * fuzzWeight.v[0];
				R.Note("APPROXIMATED " + fuzzWeightName + " " + Fmt(fuzzWeight.v[0]) +
				       " folded into sheenColor (MaterialDesc has no sheen weight; zero weight = black = no cost)");
			}
			else if (fuzzColor.kind == ValueKind::Literal) {
				// Colour without weight. OpenPBR's default weight is 0, so this
				// material has NO fuzz -- an authoring mistake worth naming,
				// because it looks set in the DCC and renders as nothing.
				R.Note(fuzzColorName + " is set but " +
				       "fuzz_weight/sheen_weight is not; OpenPBR's default weight is 0, so no sheen was imported");
			}

			e = R.Read(shader, { "fuzz_roughness", "sheen_roughness" }, m);
			if (e.kind == ValueKind::Literal) d.sheenRoughness = e.v[0];

			// --- EMISSION ----------------------------------------------------
			Eval emColor = R.Read(shader, { "emission_color" }, m);
			Eval emLum   = R.Read(shader, { "emission_luminance", "emission" }, m);
			const std::string emLumName = m ? m.name : std::string();

			if (emLum.kind == ValueKind::Literal) {
				glm::vec3 c(1.0f);
				if (emColor.kind == ValueKind::Literal && emColor.count >= 3)
					c = { emColor.v[0], emColor.v[1], emColor.v[2] };

				// standard_surface's `emission` is a unitless weight. OpenPBR's
				// `emission_luminance` is in nits and routinely reaches 1e3 for
				// a display or 1e5 for the sun. X3 has no exposure control until
				// Phase 11 and light intensities default to a unitless 1.0, so
				// the nit value is passed through and scaled only by the caller's
				// explicit option. A hidden divisor here would be unfindable.
				const bool photometric = (emLumName == "emission_luminance");
				const float strength = photometric ? emLum.v[0] * opt.emissionLuminanceScale : emLum.v[0];
				d.emission = { c.x, c.y, c.z, strength };

				if (photometric)
					R.Note("APPROXIMATED emission_luminance " + Fmt(emLum.v[0]) +
					       " nits -> emission.w " + Fmt(strength) +
					       " (X3 has no photometric pipeline; scale = " + Fmt(opt.emissionLuminanceScale) + ")");
			}
			else if (emColor.kind == ValueKind::Literal) {
				R.Note("emission_color is set but emission_luminance is not; OpenPBR's default luminance "
				       "is 0, so no emission was imported");
			}

			if (emColor.kind == ValueKind::Texture)
				R.RecordTexture("emission_color", emColor, /*defaultSRGB*/ true, &d.emissiveTex);

			// --- OPACITY ------------------------------------------------------
			e = R.Read(shader, { "geometry_opacity", "opacity" }, m);
			if (e.kind == ValueKind::Literal) {
				if (e.count >= 3) {
					// OpenPBR opacity is a colour: per-channel cutout. One alpha
					// channel cannot hold it, so average and say so.
					const float avg = (e.v[0] + e.v[1] + e.v[2]) / 3.0f;
					d.color.a = avg;
					if (!(e.v[0] == e.v[1] && e.v[1] == e.v[2]))
						R.Note("APPROXIMATED " + m.name + " is per-channel (" + Fmt(e.v[0]) + ", " +
						       Fmt(e.v[1]) + ", " + Fmt(e.v[2]) + "); averaged to alpha " + Fmt(avg));
				}
				else {
					d.color.a = e.v[0];
				}
			}

			// --- NORMAL -------------------------------------------------------
			e = R.Read(shader, { "geometry_normal", "normal" }, m);
			if (e.kind == ValueKind::Texture) {
				R.RecordTexture(m.name, e, /*defaultSRGB*/ false, &d.normalTex);
				if (e.hasNormalScale) d.normalScale = e.normalScale;
			}
			else if (e.kind == ValueKind::Graph) {
				R.Note("DROPPED " + m.name + ": driven by an unevaluated " + e.graphCategory +
				       " graph rather than an image");
			}

			// --- everything the document set that we did not consume ---------
			// Data-driven rather than a static list, so a document that never
			// mentions transmission is never told its transmission was dropped,
			// and a channel added by a future OpenPBR revision surfaces here
			// instead of vanishing.
			for (const XmlNode& c : shader.children) {
				if (!IsInputElement(c)) continue;
				const std::string_view name = c.AttrOr("name");
				if (name.empty() || R.consumed.count(std::string(name))) continue;

				if (const char* why = DropReasonFor(name))
					R.Note("DROPPED " + std::string(name) + ": " + why);
				else
					R.Note("DROPPED " + std::string(name) + ": not recognised by this importer");
			}
		}

		bool IsSurfaceShaderCategory(std::string_view category) {
			return category == "open_pbr_surface" || category == "standard_surface";
		}

	} // namespace

	// =========================================================================

	MtlxImportResult ImportMaterialXFromString(std::string_view xml, const MtlxImportOptions& options) {
		MtlxImportResult result;

		XmlNode root;
		XmlReader reader(xml);
		if (!reader.Parse(root)) {
			result.error = "MaterialX parse error: " + reader.Error();
			return result;
		}
		if (root.name != "materialx") {
			result.error = "root element is <" + root.name + ">, expected <materialx>";
			return result;
		}

		// The version gates nothing -- every difference between 1.37 and 1.39
		// that matters here is handled by accepting both spellings -- but an
		// unexpected version in the log is the first thing anyone will want when
		// a future document reads as empty.
		const std::string_view version = root.AttrOr("version");
		if (!version.empty() && version != "1.38" && version != "1.39" && version != "1.37")
			result.warnings.push_back("document version '" + std::string(version) +
			                          "' is outside the range this importer was written against (1.37-1.39)");

		Doc doc;
		doc.root = &root;
		BuildScopes(doc);

		// Surface shaders are document-level nodes. A <surfacematerial> that
		// references one supplies the name an artist will recognise; without it
		// the node name has to do.
		for (const XmlNode& node : root.children) {
			if (!IsSurfaceShaderCategory(node.name)) {
				if (node.name == "UsdPreviewSurface" || node.name == "gltf_pbr")
					result.warnings.push_back("node '" + std::string(node.AttrOr("name")) + "' is a " +
					                          node.name + "; only OpenPBR and Standard Surface are imported");
				continue;
			}

			MtlxMaterial mat;
			mat.shaderNode.assign(node.AttrOr("name"));
			mat.shaderCategory = node.name;
			mat.name = mat.shaderNode;

			for (const XmlNode& c : root.children) {
				if (c.name != "surfacematerial" && c.name != "material") continue;
				const XmlNode* surf = FindInput(c, "surfaceshader");
				if (surf && surf->AttrOr("nodename") == mat.shaderNode) {
					mat.name.assign(c.AttrOr("name", mat.shaderNode));
					break;
				}
			}

			ReduceShader(doc, node, options, mat, result.warnings);
			result.materials.push_back(std::move(mat));
		}

		if (result.materials.empty()) {
			// Not an error: a library include or a document of nodegraphs is a
			// legitimate .mtlx. The caller decides whether nothing found is a
			// failure for its purpose.
			result.warnings.push_back("no OpenPBR or Standard Surface shader node found in the document");
		}

		result.ok = true;
		return result;
	}

	MtlxImportResult ImportMaterialXFile(const std::filesystem::path& path, MtlxImportOptions options) {
		MtlxImportResult result;

		std::ifstream file(path, std::ios::binary);
		if (!file) {
			result.error = "cannot open " + path.string();
			return result;
		}
		std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

		// Relative texture paths in a .mtlx are relative to the DOCUMENT, not to
		// wherever the bake happened to be launched from. Resolving against the
		// process cwd is the classic asset bug that works for whoever authored
		// the file and for nobody else.
		if (options.documentDir.empty())
			options.documentDir = path.parent_path();

		result = ImportMaterialXFromString(contents, options);
		if (!result.error.empty())
			result.error = path.string() + ": " + result.error;
		return result;
	}

}
