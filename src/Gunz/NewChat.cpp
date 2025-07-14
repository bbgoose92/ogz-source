#include "stdafx.h"
#include "NewChat.h"
#include "RGMain.h"
#include "ZCharacterManager.h"
#include "ZInput.h"
#include "Config.h"
#include "defer.h"
#include "MClipboard.h"
#include "CodePageConversion.h"
#include "MPicture.h"
#include <algorithm>

const std::wstring Chat::MAIN_TAB_NAME = L"Main";

static std::wstring to_lower_wstring(const std::wstring& str) {
	std::wstring lower_str = str;
	std::transform(lower_str.begin(), lower_str.end(), lower_str.begin(),
		[](wchar_t c) { return std::tolower(c); });
	return lower_str;
}

namespace ResizeFlagsType
{
	enum
	{
		X1 = 1 << 0,
		Y1 = 1 << 1,
		X2 = 1 << 2,
		Y2 = 1 << 3,
	};
}


enum class FormatSpecifierType {
	Unknown = -1,
	Wrap,
	Linebreak,
	Color,
	Default,
	Bold,
	Italic,
	BoldItalic,
	Underline,
	Strikethrough,
	Emoji,
};

struct FormatSpecifier {
	int nStartPos;
	FormatSpecifierType ft;
	D3DCOLOR Color;
	std::wstring EmojiName;

	FormatSpecifier(int nStart, D3DCOLOR c) : nStartPos(nStart), ft(FormatSpecifierType::Color), Color(c) { }
	FormatSpecifier(int nStart, FormatSpecifierType type) : nStartPos(nStart), ft(type) { }
	FormatSpecifier(int nStart, std::wstring name) : nStartPos(nStart), ft(FormatSpecifierType::Emoji), EmojiName(std::move(name)) { }
};


struct ChatMessage {
	Chat::TimeType Time{};
	std::wstring OriginalMsg;
	std::wstring ProcessedMsg;
	u32 DefaultColor;
	std::vector<FormatSpecifier> FormatSpecifiers;
	int Lines{};

	void SubstituteFormatSpecifiers(const std::map<std::wstring, MBitmap*>& EmojiMap);

	int GetLines() const {
		return Lines;
	}

	void ClearWrappingLineBreaks() {
		erase_remove_if(FormatSpecifiers, [&](auto&& x) { return x.ft == FormatSpecifierType::Wrap; });
	}

	const FormatSpecifier* GetLineBreak(int n) const {
		int i = 0;
		for (auto it = FormatSpecifiers.begin(); it != FormatSpecifiers.end(); it++) {
			if (it->ft == FormatSpecifierType::Wrap || it->ft == FormatSpecifierType::Linebreak) {
				if (i == n)
					return &*it;

				i++;
			}
		}

		return 0;
	}

	auto AddWrappingLineBreak(int n) {
		assert(n >= 0);
		if (n < 0)
			n = 0;

		if (FormatSpecifiers.empty()) {
			FormatSpecifiers.emplace_back(n, FormatSpecifierType::Wrap);
			return std::prev(FormatSpecifiers.end());
		}

		for (auto it = FormatSpecifiers.rbegin(); it != FormatSpecifiers.rend(); it++) {
			if (it->nStartPos < n) {
				return FormatSpecifiers.insert(it.base(), FormatSpecifier(n, FormatSpecifierType::Wrap));
			}
		}

		return FormatSpecifiers.insert(FormatSpecifiers.begin(), FormatSpecifier(n, FormatSpecifierType::Wrap));
	}
};

namespace EmphasisType
{
	enum
	{
		Default = 0,
		Italic = 1 << 0,
		Bold = 1 << 1,
		Underline = 1 << 2,
		Strikethrough = 1 << 3,
	};
}


struct LineSegmentInfo
{
	enum class SegmentType { Text, Emoji };
	SegmentType Type{ SegmentType::Text };
	MBitmap* pEmojiBitmap{ nullptr };

	int ChatMessageIndex;
	u16 Offset;
	u16 LengthInCharacters;
	u16 PixelOffsetX;
	struct {
		u16 IsStartOfLine : 1;
		u16 Emphasis : 15;
	};
	u32 TextColor;
};

static constexpr int MAX_INPUT_LENGTH = 230;
void ChatMessage::SubstituteFormatSpecifiers(const std::map<std::wstring, MBitmap*>& EmojiMap)
{
	auto CharToFT = [&](char c) {
		switch (c) {
		case 'b': return FormatSpecifierType::Bold;
		case 'i': return FormatSpecifierType::Italic;
		case 's': return FormatSpecifierType::Strikethrough;
		case 'u': return FormatSpecifierType::Underline;
		default:  return FormatSpecifierType::Unknown;
		};
	};

	const auto npos = std::wstring::npos;
	bool Erased = false;

	for (auto Pos = ProcessedMsg.find_first_of(L"^[", 0);
		Pos != npos && Pos <= ProcessedMsg.length() - 2;
		Pos = Pos < ProcessedMsg.length() ? ProcessedMsg.find_first_of(L"^[", Erased ? Pos : Pos + 1) : npos)
	{
		Erased = false;

		auto Erase = [&](std::wstring::size_type Count) {
			ProcessedMsg.erase(Pos, Count);
			Erased = true;
		};

		if (Pos + 1 >= ProcessedMsg.length()) continue;
		auto CurrentChar = ProcessedMsg[Pos];

		if (CurrentChar == '^')
		{
			auto NextChar = ProcessedMsg[Pos + 1];
			if (isdigit(NextChar))
			{
				FormatSpecifiers.emplace_back(Pos, MMColorSet[NextChar - '0']);
				Erase(2);
			}
			else if (NextChar == '#')
			{
				auto ishexdigit = [&](auto c) {
					c = tolower(c);
					return isdigit(c) || (c >= 'a' && c <= 'f');
				};

				auto ColorStart = Pos + 2;
				auto ColorEnd = ColorStart;
				while (ColorEnd < ProcessedMsg.length() && ColorEnd - ColorStart < 8 && ishexdigit(ProcessedMsg[ColorEnd])) {
					++ColorEnd;
				}

				auto Distance = ColorEnd - ColorStart;
				if (Distance == 8) {
					wchar_t ColorString[32];
					strncpy_safe(ColorString, &ProcessedMsg[ColorStart], Distance);
					wchar_t* endptr;
					auto Color = static_cast<D3DCOLOR>(wcstoul(ColorString, &endptr, 16));
					FormatSpecifiers.emplace_back(Pos, Color);
					Erase(ColorEnd - Pos);
				}
			}
		}
		else if (CurrentChar == '[')
		{
			auto EndBracket = ProcessedMsg.find_first_of(L"]", Pos + 1);
			if (EndBracket == npos) continue;

			auto Distance = EndBracket - Pos;
			if (ProcessedMsg[Pos + 1] == '/' && (Distance == 2 || Distance == 3))
			{
				FormatSpecifiers.emplace_back(Pos, FormatSpecifierType::Default);
			}
			else
			{
				auto ft = CharToFT(ProcessedMsg[Pos + 1]);
				if (ft != FormatSpecifierType::Unknown)
					FormatSpecifiers.emplace_back(Pos, ft);
			}
			Erase(Distance + 1);
		}
	}

	size_t SearchPos = 0;
	while ((SearchPos = ProcessedMsg.find(L':', SearchPos)) != npos) {
		auto EndPos = ProcessedMsg.find(L':', SearchPos + 1);
		if (EndPos == npos) {
			break;
		}

		std::wstring EmojiName = ProcessedMsg.substr(SearchPos + 1, EndPos - (SearchPos + 1));
		if (!EmojiName.empty() && EmojiMap.count(EmojiName))
		{
			if (EndPos == ProcessedMsg.length() - 1)
			{
				ProcessedMsg.replace(SearchPos, EndPos - SearchPos + 1, L"\uFFFC ");
			}
			else
			{
				ProcessedMsg.replace(SearchPos, EndPos - SearchPos + 1, L"\uFFFC");
			}

			FormatSpecifiers.emplace_back(SearchPos, EmojiName);

			SearchPos++;
		}
		else {
			SearchPos++;
		}
	}
}

void Chat::InitializeEmojis()
{
	
	// Pair format: { "EmojiNameWithoutColons", "emoji_filename.png" }
	const std::vector<std::pair<const wchar_t*, const char*>> emojiList = {
		{ L"sweat",   "monkas.png" },
		{ L"sadge",    "sadge.png" },
		{ L"yes", "pepyes.png" },
		{ L"no",   "pepno.png" },
		{ L"cool",   "cool.png" },
		{ L"angry",   "angry.png" },
		{ L"smug",   "smug.png" },
		{ L"think",   "think.png" },
		{ L"laugh",   "laugh.png" },
		{ L"wtf",   "wtf.png" },
		{ L"lost",   "lost.png" },
		{ L"wave",   "wave.png" },
		{ L"imok",   "imok.png" },
		{ L"finger",   "finger.png" },
		{ L"giggle",   "giggle.png" },
		{ L"hm",      "pephm.png" }
		// --- Add your new emoji spots here ---
		// { L"newEmojiName", "new_emoji_file.png" },
		// { L"anotherOne",   "another_one.png" },
	};

	// Loop through the list and load each emoji
	for (const auto& emoji : emojiList) {
		MBitmap* pBitmap = MBitmapManager::Get(emoji.second); // Get bitmap from filename
		if (pBitmap) {
			m_EmojiMap[emoji.first] = pBitmap; // Map the emoji name to the bitmap
		}
	}
}


Chat::Chat(const std::string& FontName, bool BoldFont, int FontSize)
	: FontName{ FontName }, BoldFont{ BoldFont }, FontSize{ FontSize }
{
	const auto ScreenWidth = RGetScreenWidth();
	const auto ScreenHeight = RGetScreenHeight();
	m_bLeftButtonDownLastFrame = false;
	Border.x1 = 10;
	Border.y1 = double(1080 - 280) / 1080 * ScreenHeight;
	Border.x2 = (double)700 / 1920 * ScreenWidth;
	Border.y2 = double(1080 - 40) / 1080 * ScreenHeight;

	Cursor.x = ScreenWidth / 2;
	Cursor.y = ScreenHeight / 2;

	const auto Scale = 1.f;
	DefaultFont.Create("NewChatFont", FontName.c_str(),
		int(float(FontSize) / 1080 * RGetScreenHeight() + 0.5), Scale, BoldFont);
	ItalicFont.Create("NewChatItalicFont", FontName.c_str(),
		int(float(FontSize) / 1080 * RGetScreenHeight() + 0.5), Scale, BoldFont, true);

	FontHeight = DefaultFont.GetHeight();

	// --- FIX: Use a normalized key for the main tab ---
	std::wstring main_key = to_lower_wstring(MAIN_TAB_NAME);
	m_Tabs[main_key] = ChatTab{};
	m_Tabs[main_key].Name = MAIN_TAB_NAME; // Keep original case for display
	m_sActiveTabName = main_key;           // Set active tab to the normalized key
}

Chat::~Chat() = default;

ChatTab& Chat::GetActiveTab()
{
	return m_Tabs.at(m_sActiveTabName);
}

const ChatTab& Chat::GetActiveTab() const
{
	return m_Tabs.at(m_sActiveTabName);
}

void Chat::EnableInput(bool Enable, bool ToTeam) {
	InputEnabled = Enable;
	TeamChat = ToTeam;

	if (Enable) {
		// Mark all current notifications as acknowledged by the user.
		for (auto& pair : m_Tabs) {
			pair.second.bHasBeenAcknowledged = true;
		}

		m_bNotificationsMuted = false;

		InputField.clear();

		CaretPos = -1;

		SetCursorPos(RGetScreenWidth() / 2, RGetScreenHeight() / 2);
	}
	else {
		ZGetInput()->ResetRotation();

		SelectionState = SelectionStateType{};
	}

	ZGetGameInterface()->SetCursorEnable(Enable);

	ZPostPeerChatIcon(Enable);
}

void Chat::OutputChatMsg(const char* Msg) {
	OutputChatMsg(Msg, TextColor);
}

void Chat::OutputChatMsg(const char* szMsg, u32 dwColor)
{
	wchar_t WideMsg[4096];
	if (CodePageConversion<CP_UTF8>(WideMsg, szMsg) == ConversionError)
	{
		MLog("Chat::OutputChatMsg -- Conversion error\n");
		assert(false);
		return;
	}

	std::wstring msg_str(WideMsg);
	const std::wstring incoming_whisper_prefix = L"Whispering (";
	const std::wstring outgoing_whisper_prefix = L"(To ";

	std::wstring targetTabKey = to_lower_wstring(MAIN_TAB_NAME);
	std::wstring displayName = MAIN_TAB_NAME;
	std::wstring finalMsg = msg_str;

	bool is_incoming = (msg_str.rfind(incoming_whisper_prefix, 0) == 0);
	bool is_outgoing = (msg_str.rfind(outgoing_whisper_prefix, 0) == 0);

	if (is_incoming || is_outgoing)
	{
		size_t name_start = is_incoming ? incoming_whisper_prefix.length() : outgoing_whisper_prefix.length();
		size_t name_end = msg_str.find(L")", name_start);
		size_t msg_start = msg_str.find(L": ", name_end);

		if (name_end != std::wstring::npos && msg_start != std::wstring::npos)
		{
			// --- NORMALIZATION LOGIC ---
			// Store the original name for display
			displayName = msg_str.substr(name_start, name_end - name_start);
			// Use the lowercase version as the key
			targetTabKey = to_lower_wstring(displayName);
			// --- END NORMALIZATION LOGIC ---

			finalMsg = msg_str.substr(msg_start + 2);
		}
	}

	// Use the normalized key for all map operations
	if (m_Tabs.find(targetTabKey) == m_Tabs.end()) {
		m_Tabs[targetTabKey] = ChatTab{};
		// Store the original-cased name for display
		m_Tabs[targetTabKey].Name = displayName;
	}

	ChatTab& targetTab = m_Tabs[targetTabKey];

	// --- START of CHANGE: Add "You:" and "Them:" prefixes to whispers ---

	if (is_incoming) {
		finalMsg = L"Them:" + finalMsg;
	}
	else if (is_outgoing) {
		finalMsg = L"You:" + finalMsg;
	}

	// --- END of CHANGE ---

	targetTab.Messages.emplace_back();
	auto&& Msg = targetTab.Messages.back();
	Msg.Time = GetTime();
	Msg.OriginalMsg = finalMsg;
	Msg.ProcessedMsg = finalMsg;
	Msg.DefaultColor = dwColor;

	targetTab.bLayoutIsDirty = true;

	if (is_incoming && to_lower_wstring(m_sActiveTabName) != targetTabKey) {
		targetTab.UnreadCount++;
		targetTab.bHasBeenAcknowledged = false; // This resets the acknowledged status for the new message.
	}

	if (to_lower_wstring(m_sActiveTabName) == targetTabKey)
	{
		if (!targetTab.Messages.empty()) {
			NumNewlyAddedLines = targetTab.Messages.back().GetLines();
			if (targetTab.ScrollOffsetLines > 0) {
				targetTab.ScrollOffsetLines += NumNewlyAddedLines;
			}
			if (ChatLinesPixelOffsetY <= 0) {
				ChatLinesPixelOffsetY = FontHeight;
			}
		}
	}
	m_LastMessageTime = GetTime();
}


void Chat::Scale(double WidthRatio, double HeightRatio) {
	Border.x1 *= WidthRatio;
	Border.x2 *= WidthRatio;
	Border.y1 *= HeightRatio;
	Border.y2 *= HeightRatio;

	ResetFonts();

	for (auto& pair : m_Tabs) {
		
		pair.second.bLayoutIsDirty = true;
	}
}

void Chat::Resize(int nWidth, int nHeight)
{
	Border.x1 = 10;
	
	Border.y1 = double(1080 - 280) / 1080 * RGetScreenHeight(); 
	Border.x2 = (double)700 / 1920 * RGetScreenWidth();
	Border.y2 = double(1080 - 40) / 1080 * RGetScreenHeight();  

	ResetFonts();

	for (auto& pair : m_Tabs) {
		pair.second.bLayoutIsDirty = true;
	}
}

void Chat::ClearHistory()
{
	for (auto& pair : m_Tabs) {
		auto& tab = pair.second; 
		tab.Messages.clear();
		tab.LineSegments.clear();
		tab.ScrollOffsetLines = 0;
		tab.TotalLinesInHistory = 0;
		tab.bLayoutIsDirty = true;
	}
	NumNewlyAddedLines = 0;
	ChatLinesPixelOffsetY = 0;
}

Chat::TimeType Chat::GetTime()
{
	return ZGetApplication()->GetTime();
}

bool Chat::CursorInRange(int x1, int y1, int x2, int y2) {
	return Cursor.x > x1 && Cursor.x < x2&& Cursor.y > y1 && Cursor.y < y2;
}

int Chat::GetTextLength(MFontR2 & Font, const wchar_t* Format, ...)
{
	wchar_t buf[1024];
	va_list va;
	va_start(va, Format);
	vsprintf_safe(buf, Format, va);
	va_end(va);
	return Font.GetWidth(buf);
}

struct CaretType
{
	int TotalTextHeight;
	v2i CaretPos;
};
static CaretType GetCaretPos(MFontR2 & Font, const wchar_t* Text, int CaretPos, int Width)
{
	CaretType ret{ 1, { 0, 1 } };
	v2i Cursor{ 0, 1 };
	for (auto c = Text; *c != 0; ++c)
	{
		auto CharWidth = Font.GetWidth(c, 1);

		Cursor.x += CharWidth;
		if (Cursor.x > Width)
		{
			++Cursor.y;
			Cursor.x = CharWidth;
		}

		auto Distance = c - Text;
		if (Distance == CaretPos)
			ret.CaretPos = Cursor;
	}
	ret.TotalTextHeight = Cursor.y;
	return ret;
}

std::pair<bool, v2i> Chat::GetPos(const ChatTab & tab, const ChatMessage & c, u32 Pos)
{
	std::pair<bool, v2i> ret{ false, {0, 0} };
	if (Pos > c.ProcessedMsg.length())
		return ret;

	D3DRECT Output = GetOutputRect();

	int Limit = (Output.y2 - Output.y1 - 10) / FontHeight;

	int nLines = 0;

	for (int i = tab.Messages.size() - 1; nLines < Limit && i >= 0; i--) {
		auto& cl = tab.Messages.at(i);

		if (&c == &cl) {
			int nOffset = 0;

			if (c.GetLines() == 1) {
				ret.second.y = Output.y2 - 5 - (nLines)*FontHeight - FontHeight * .5;
			}
			else {
				int nLine = 0;

				for (int i = 0; i < c.GetLines() - 1; i++) {
					if (int(Pos) < c.GetLineBreak(i)->nStartPos)
						break;

					nLine++;
				}

				ret.second.y = Output.y2 - 5 - (nLines - nLine) * FontHeight - FontHeight * .5;

				if (nLine > 0)
					nOffset = c.GetLineBreak(nLine - 1)->nStartPos;
			}

			ret.second.x = Output.x1 + 5 + GetTextLength(DefaultFont, L"%.*s_", Pos - nOffset,
				&c.ProcessedMsg.at(nOffset)) - GetTextLength(DefaultFont, L"_");

			ret.first = true;
			return ret;
		}

		nLines += cl.GetLines();
	}

	return ret;
}

bool Chat::OnEvent(MEvent* pEvent) {
	// TAB CLICK HANDLING
	if (pEvent->nMessage == MWM_LBUTTONDOWN) {
		int tabX = Border.x1 + 5;
		const int tabY = Border.y1 - 20;
		const int tabHeight = 20;
		const std::wstring main_key = to_lower_wstring(MAIN_TAB_NAME);


		if (m_Tabs.count(main_key))
		{
			const auto& tabData = m_Tabs.at(main_key);
			int tabWidth = DefaultFont.GetWidth(tabData.Name.c_str()) + 10;
			if (CursorInRange(tabX, tabY, tabX + tabWidth, tabY + tabHeight)) {
				m_sActiveTabName = main_key;
				m_Tabs.at(main_key).UnreadCount = 0;
				m_Tabs.at(main_key).bHasBeenAcknowledged = true; // Mark as acknowledged
				SelectionState = {};
				return true;
			}
			tabX += tabWidth + 2;
		}


		for (auto const& pair : m_Tabs) {
			const auto& key = pair.first;
			if (key == main_key) continue;

			const auto& tabData = pair.second;
			bool isInputActive = IsInputEnabled();


			bool shouldCheckThisTab = (tabData.UnreadCount > 0) || isInputActive;
			if (!shouldCheckThisTab) continue;

			std::wstring textToDraw = tabData.Name;
			if (tabData.UnreadCount > 0) {
				textToDraw += L" (" + std::to_wstring(tabData.UnreadCount) + L")";
			}
			int tabWidth = DefaultFont.GetWidth(textToDraw.c_str()) + 10;

			if (CursorInRange(tabX, tabY, tabX + tabWidth, tabY + tabHeight)) {
				m_sActiveTabName = key;
				m_Tabs.at(key).UnreadCount = 0;
				m_Tabs.at(key).bHasBeenAcknowledged = true; // Mark as acknowledged
				SelectionState = {};
				return true;
			}
			tabX += tabWidth + 2;
		}
	}


	if (pEvent->nMessage == MWM_RBUTTONDOWN) {
		int tabX = Border.x1 + 5;
		const int tabY = Border.y1 - 20;
		const int tabHeight = 20;
		const std::wstring main_key = to_lower_wstring(MAIN_TAB_NAME);
		bool isMainChatFading = false;


		if (m_Tabs.count(to_lower_wstring(MAIN_TAB_NAME))) {
			const auto& mainTab = m_Tabs.at(to_lower_wstring(MAIN_TAB_NAME));
			for (const auto& msg : mainTab.Messages) {
				if (GetTime() < msg.Time + FadeTime) {
					isMainChatFading = true;
					break;
				}
			}
		}


		std::vector<std::wstring> tabDrawOrder;
		if (m_Tabs.count(main_key)) {
			tabDrawOrder.push_back(main_key);
		}
		for (auto const& pair : m_Tabs) {
			if (pair.first == main_key) continue;
			tabDrawOrder.push_back(pair.first);
		}


		for (const auto& key : tabDrawOrder) {
			const auto& tabData = m_Tabs.at(key);
			bool isInputActive = IsInputEnabled();

			bool isMainTabVisible = isInputActive || isMainChatFading;
			bool shouldCheckThisTab = (key == main_key) ? isMainTabVisible : ((tabData.UnreadCount > 0) || isInputActive);

			if (!shouldCheckThisTab) continue;

			std::wstring textToDraw = tabData.Name;
			if (tabData.UnreadCount > 0) {
				textToDraw += L" (" + std::to_wstring(tabData.UnreadCount) + L")";
			}
			int tabWidth = DefaultFont.GetWidth(textToDraw.c_str()) + 10;

			if (CursorInRange(tabX, tabY, tabX + tabWidth, tabY + tabHeight)) {
				// Prevent the "Main" tab from being closed.
				if (key != main_key) {
					if (m_sActiveTabName == key) {
						m_sActiveTabName = main_key;
					}
					m_Tabs.erase(key);
					return true;
				}
			}
			tabX += tabWidth + 2;
		}
	}


	if (pEvent->nMessage == MWM_MOUSEWHEEL)
	{
		D3DRECT TotalRect = GetTotalRect();
		if (CursorInRange(TotalRect.x1, TotalRect.y1, TotalRect.x2, TotalRect.y2))
		{
			ChatTab& activeTab = GetActiveTab();
			int WheelDelta = pEvent->nDelta;
			const int ScrollAmount = 3;

			if (WheelDelta > 0) {
				activeTab.ScrollOffsetLines += ScrollAmount;
			}
			else {
				activeTab.ScrollOffsetLines -= ScrollAmount;
			}

			auto OutputRect = GetOutputRect();
			int VisibleLines = max(1, static_cast<int>((OutputRect.y2 - OutputRect.y1 - 10) / FontHeight));
			int MaxScrollOffset = max(0, activeTab.TotalLinesInHistory - VisibleLines);
			activeTab.ScrollOffsetLines = max(0, min(activeTab.ScrollOffsetLines, MaxScrollOffset));

			NumNewlyAddedLines = 0;
			ChatLinesPixelOffsetY = 0;

			return true;
		}
	}

	const auto ActionPressed = pEvent->nMessage == MWM_ACTIONPRESSED;
	const auto CharMessage = pEvent->nMessage == MWM_CHAR;

	bool ChatPressed = false;
	{
		static bool IgnoreNextChatActionKey = false;

		auto&& Key = ZGetConfiguration()->GetKeyboard()->ActionKeys[ZACTION_CHAT];
		if (InputEnabled)
		{
			ChatPressed = CharMessage && pEvent->nKey == VK_RETURN;
			if (Key.nVirtualKey == DIK_RETURN || Key.nVirtualKeyAlt == DIK_RETURN)
				IgnoreNextChatActionKey = true;
		}
		else
		{
			auto ChatActionKeyPressed = ActionPressed && pEvent->nKey == ZACTION_CHAT;
			if (IgnoreNextChatActionKey && ChatActionKeyPressed)
			{
				IgnoreNextChatActionKey = false;
			}
			else
			{
				ChatPressed = ChatActionKeyPressed;
			}
		}
	}

	const auto TeamChatPressed = !InputEnabled && ActionPressed && pEvent->nKey == ZACTION_TEAMCHAT;

	if (ChatPressed || TeamChatPressed)
	{
		if (InputEnabled && ChatPressed && !InputField.empty())
		{
			std::wstring finalMessage = InputField;

			if (m_sActiveTabName != to_lower_wstring(MAIN_TAB_NAME))
			{
				const std::wstring& displayName = GetActiveTab().Name;
				finalMessage = L"/whisper " + displayName + L" " + InputField;
			}

			char MultiByteString[1024];
			CodePageConversion<CP_UTF8>(MultiByteString, finalMessage.c_str());

			ZGetGameInterface()->GetChat()->Input(MultiByteString);

			InputHistory.push_back(InputField);
			CurInputHistoryEntry = InputHistory.size();

			InputField.clear();
			CaretPos = -1;

			GetActiveTab().ScrollOffsetLines = 0;
		}

		EnableInput(!InputEnabled, TeamChatPressed);
	}

	if (pEvent->nMessage == MWM_KEYDOWN) {
		ChatTab& activeTab = GetActiveTab();
		switch (pEvent->nKey) {

		case VK_HOME:
			CaretPos = -1;
			break;

		case VK_END:
			CaretPos = InputField.length() - 1;
			break;

		case VK_TAB:
		{
			size_t StartPos = InputField.rfind(' ');
			if (StartPos == std::string::npos)
				StartPos = 0;
			else
				StartPos++;

			if (StartPos == InputField.length())
				break;

			size_t PartialNameLength = InputField.size() - StartPos;

			auto PartialName = InputField.data() + StartPos;

			for (auto& it : *ZGetCharacterManager())
			{
				ZCharacter& Player = *it.second;
				const char* PlayerName = Player.GetProperty()->szName;
				size_t PlayerNameLength = strlen(PlayerName);

				if (PlayerNameLength < PartialNameLength)
					continue;

				wchar_t WidePlayerName[256];
				auto len = CodePageConversion<CP_ACP>(WidePlayerName, PlayerName);
				if (len == ConversionError)
				{
					MLog("Chat::OnEvent -- Conversion error while autocompleting name %s\n", PlayerName);
					assert(false);
					continue;
				}

				if (!_wcsnicmp(PartialName, WidePlayerName, PartialNameLength))
				{
					if (InputField.size() + PlayerNameLength - PartialNameLength > MAX_INPUT_LENGTH)
						break;

					for (size_t i = 0; i < PartialNameLength; i++)
						InputField.erase(InputField.size() - 1);

					InputField.append(WidePlayerName);
					CaretPos += PlayerNameLength - PartialNameLength;
					break;
				}
			}
		}

		break;

		case VK_PRIOR:
		{
			auto OutputRect = GetOutputRect();
			int VisibleLines = max(1, static_cast<int>((OutputRect.y2 - OutputRect.y1 - 10) / FontHeight));
			activeTab.ScrollOffsetLines -= VisibleLines;
			NumNewlyAddedLines = 0;
			ChatLinesPixelOffsetY = 0;
			break;
		}

		case VK_NEXT:
		{
			auto OutputRect = GetOutputRect();
			int VisibleLines = max(1, static_cast<int>((OutputRect.y2 - OutputRect.y1 - 10) / FontHeight));
			activeTab.ScrollOffsetLines += VisibleLines;
			NumNewlyAddedLines = 0;
			ChatLinesPixelOffsetY = 0;
			break;
		}

		case VK_UP:
			if (CurInputHistoryEntry > 0) {
				CurInputHistoryEntry--;
				InputField.assign(InputHistory.at(CurInputHistoryEntry));
				CaretPos = InputHistory.at(CurInputHistoryEntry).length() - 1;
			}
			break;

		case VK_DOWN:
			if (CurInputHistoryEntry < int(InputHistory.size()) - 1) {
				CurInputHistoryEntry++;
				auto&& strEntry = InputHistory.at(CurInputHistoryEntry);
				InputField.assign(strEntry);
				CaretPos = strEntry.length() - 1;
			}
			else {
				InputField.clear();
				CaretPos = -1;
			}

			break;

		case VK_LEFT:
			if (CaretPos >= 0)
				CaretPos--;
			break;

		case VK_RIGHT:
			if (CaretPos < int(InputField.length()) - 1)
				CaretPos++;
			break;

		case 'V':
		{
			if (pEvent->bCtrl) // If Ctrl is held, do the paste action
			{
				wchar_t Clipboard[256];
				MClipboard::Get(g_hWnd, Clipboard, std::size(Clipboard));
				if (InputField.length() + wcslen(Clipboard) > MAX_INPUT_LENGTH)
				{
					InputField.append(Clipboard, Clipboard + MAX_INPUT_LENGTH - InputField.length());
				}
				else
				{
					InputField += Clipboard;
				}
			}
			else // Otherwise, if V is pressed alone
			{
				if (!IsInputEnabled()) // And if the chat window is closed
				{
					m_bNotificationsMuted = !m_bNotificationsMuted; // Toggle the flag
				}
			}
			break;
		}

		};
	}
	else if (pEvent->nMessage == MWM_CHAR) {
		switch (pEvent->nKey) {

		case VK_TAB:
		case VK_RETURN:
			break;

		case VK_BACK:
			if (CaretPos >= 0) {
				InputField.erase(CaretPos, 1);
				CaretPos--;
			}
			break;
		case VK_ESCAPE:
			Resize(RGetScreenWidth(), RGetScreenHeight());

			EnableInput(false, false);
			break;

		default:
			if (InputField.length() < MAX_INPUT_LENGTH) {
				if (pEvent->nKey < 27)
					break;

				InputField.insert(CaretPos + 1, 1, pEvent->nKey);

				auto SlashR = L"/r ";
				auto SlashWhisper = L"/whisper ";
				if (iequals(InputField, SlashR))
				{
					wchar_t LastSenderWide[512];
					auto* LastSender = ZGetGameInterface()->GetChat()->m_szWhisperLastSender;
					auto len = CodePageConversion<CP_ACP>(LastSenderWide, LastSender);
					if (len == ConversionError)
					{
						MLog("Chat::OnEvent -- Conversion error while handling /r on name %s\n", LastSender);
						assert(false);
						break;
					}

					InputField = SlashWhisper;
					InputField += LastSenderWide;
					InputField += ' ';
					CaretPos = InputField.length() - 1;
				}
				else
				{
					CaretPos++;
				}
			}
		};
	}

	auto ret = GetCaretPos(DefaultFont, InputField.c_str(), CaretPos, Border.x2 - (Border.x1 + 5));
	InputHeight = ret.TotalTextHeight;
	CaretCoord = ret.CaretPos;

	return true;
}

int Chat::GetTextLen(ChatMessage & cl, int Pos, int Count) {
	return GetTextLength(DefaultFont, L"_%.*s_", Count, &cl.ProcessedMsg.at(Pos)) - GetTextLength(DefaultFont, L"__");
}

int Chat::GetTextLen(const char* Msg, int Count) {
	return GetTextLength(DefaultFont, L"_%.*s_", Count, Msg) - GetTextLength(DefaultFont, L"__");
}

void Chat::OnUpdate(float TimeDelta) {
	UpdateNewMessagesAnimation(TimeDelta);

	if (!IsInputEnabled())
		return;

	ChatTab& activeTab = GetActiveTab();
	auto PrevCursorPos = Cursor;
	Cursor = MEvent::LatestPos;

	if (m_bDragAndResizeEnabled) {
		v2 MinimumSize{ 192.f * RGetScreenWidth() / 1920.f, 108.f * RGetScreenHeight() / 1080.f };

		if (ResizeFlags) {
			SelectionState = {};
			if (ResizeFlags & ResizeFlagsType::X1 &&
				Border.x1 + Cursor.x - PrevCursorPos.x < Border.x2 - MinimumSize.x) {
				Border.x1 += Cursor.x - PrevCursorPos.x;
			}
			if (ResizeFlags & ResizeFlagsType::X2 &&
				Border.x2 + Cursor.x - PrevCursorPos.x > Border.x1 + MinimumSize.x) {
				Border.x2 += Cursor.x - PrevCursorPos.x;
			}
			if (ResizeFlags & ResizeFlagsType::Y1 &&
				Border.y1 + Cursor.y - PrevCursorPos.y < Border.y2 - MinimumSize.y) {
				Border.y1 += Cursor.y - PrevCursorPos.y;
			}
			if (ResizeFlags & ResizeFlagsType::Y2 &&
				Border.y2 + Cursor.y - PrevCursorPos.y > Border.y1 + MinimumSize.y) {
				Border.y2 += Cursor.y - PrevCursorPos.y;
			}

			for (auto& pair : m_Tabs) {
				pair.second.bLayoutIsDirty = true;
			}
		}

		if (Action == ChatWindowAction::Moving) {
			Border.x1 += Cursor.x - PrevCursorPos.x;
			Border.y1 += Cursor.y - PrevCursorPos.y;
			Border.x2 += Cursor.x - PrevCursorPos.x;
			Border.y2 += Cursor.y - PrevCursorPos.y;
		}
	}

	if (Action == ChatWindowAction::Scrolling)
	{
		auto Output = GetOutputRect();
		int VisibleLines = max(1, static_cast<int>((Output.y2 - Output.y1 - 10) / FontHeight));
		float TrackHeight = static_cast<float>(Output.y2 - Output.y1);

		float RelativeCursorY = (Cursor.y - Output.y1) / TrackHeight;
		RelativeCursorY = max(0.f, min(1.f, RelativeCursorY));

		if (activeTab.TotalLinesInHistory - VisibleLines > 0)
			activeTab.ScrollOffsetLines = static_cast<int>((1.0f - RelativeCursorY) * (activeTab.TotalLinesInHistory - VisibleLines));
		else
			activeTab.ScrollOffsetLines = 0;
	}

	if (SelectionState.FromMsg && SelectionState.ToMsg &&
		MEvent::IsKeyDown(VK_CONTROL) && MEvent::IsKeyDown('C')) {
		if (OpenClipboard(g_hWnd)) {
			EmptyClipboard();

			if (SelectionState.FromMsg == SelectionState.ToMsg) {
				auto index = min(SelectionState.FromPos, SelectionState.ToPos);
				auto count = max(SelectionState.FromPos, SelectionState.ToPos) - index;
				auto str = SelectionState.FromMsg->ProcessedMsg.substr(index, count + 1);
				MClipboard::Set(g_hWnd, str);
			}
			else {
				std::wstring str;

				const ChatMessage* pStartMsg, * pEndMsg;
				int nStartPos, nEndPos;

				if (SelectionState.FromMsg < SelectionState.ToMsg) {
					pStartMsg = SelectionState.FromMsg;
					nStartPos = SelectionState.FromPos;
					pEndMsg = SelectionState.ToMsg;
					nEndPos = SelectionState.ToPos;
				}
				else {
					pStartMsg = SelectionState.ToMsg;
					nStartPos = SelectionState.ToPos;
					pEndMsg = SelectionState.FromMsg;
					nEndPos = SelectionState.FromPos;
				}

				for (auto it = activeTab.Messages.begin(); it != activeTab.Messages.end(); it++) {
					const auto* pcl = &*it;

					if (pcl < pStartMsg) continue;
					if (pcl > pEndMsg) break;

					if (pcl == pStartMsg) {
						str.append(pStartMsg->ProcessedMsg.substr(nStartPos));
					}
					else if (pcl == pEndMsg) {
						str.append(L"\n");
						str.append(pEndMsg->ProcessedMsg.substr(0, nEndPos + 1));
					}
					else {
						str.append(L"\n");
						str.append(pcl->ProcessedMsg);
					}
				}

				if (!str.empty()) {
					MClipboard::Set(g_hWnd, str);
				}
			}

			CloseClipboard();
		}
	}

	const int nBorderWidth = 5;

	bool bLeftButtonPressed = MEvent::IsKeyDown(VK_LBUTTON) && !m_bLeftButtonDownLastFrame;

	if (MEvent::IsKeyDown(VK_LBUTTON)) {
		if (Action == ChatWindowAction::None) {
			D3DRECT LockButtonRect = { Border.x1 + 5, Border.y1 - 18, Border.x1 + 5 + 12, Border.y1 - 18 + FontHeight };

			if (bLeftButtonPressed && CursorInRange(LockButtonRect.x1, LockButtonRect.y1, LockButtonRect.x2, LockButtonRect.y2))
			{
				m_bDragAndResizeEnabled = !m_bDragAndResizeEnabled;
			}
			else if (bLeftButtonPressed && CursorInRange(Border.x2 - 15, Border.y1 - 18, Border.x2 - 15 + 12, Border.y1 - 18 + FontHeight))
			{
				Resize(RGetScreenWidth(), RGetScreenHeight());
			}
			else
			{
				D3DRECT tr = GetTotalRect();
				const int ScrollbarWidth = 15;
				auto Output = GetOutputRect();
				int VisibleLines = max(1, static_cast<int>((Output.y2 - Output.y1 - 10) / FontHeight));
				D3DRECT ScrollbarTrackRect = { Output.x2 - ScrollbarWidth, Output.y1, Output.x2, Output.y2 };

				if (CursorInRange(ScrollbarTrackRect.x1, ScrollbarTrackRect.y1, ScrollbarTrackRect.x2, ScrollbarTrackRect.y2) && activeTab.TotalLinesInHistory > VisibleLines) {
					Action = ChatWindowAction::Scrolling;
					NumNewlyAddedLines = 0;
					ChatLinesPixelOffsetY = 0;
				}
				else if (m_bDragAndResizeEnabled) {
					if (CursorInRange(tr.x1 - nBorderWidth, tr.y1 - nBorderWidth, tr.x1 + nBorderWidth, tr.y2 + nBorderWidth)) ResizeFlags |= ResizeFlagsType::X1;
					if (CursorInRange(tr.x1 - nBorderWidth, tr.y1 - nBorderWidth, tr.x2 + nBorderWidth, tr.y1 + nBorderWidth)) ResizeFlags |= ResizeFlagsType::Y1;
					if (CursorInRange(tr.x2 - nBorderWidth, tr.y1 - nBorderWidth, tr.x2 + nBorderWidth, tr.y2 + nBorderWidth)) ResizeFlags |= ResizeFlagsType::X2;
					if (CursorInRange(tr.x1 - nBorderWidth, tr.y2 - nBorderWidth, tr.x2 + nBorderWidth, tr.y2 + nBorderWidth)) ResizeFlags |= ResizeFlagsType::Y2;

					if (ResizeFlags) {
						Action = ChatWindowAction::Resizing;
					}
					else if (CursorInRange(Border.x1, Border.y1 - 20, Border.x2 + 1, Border.y1)) {
						Action = ChatWindowAction::Moving;
					}
				}
			}
		}

		if (Action == ChatWindowAction::None || Action == ChatWindowAction::Selecting) {
			if (CursorInRange(Border.x1 + 5, Border.y1 + 5, Border.x2 - 5, Border.y2 - 5)) {
				if (Action != ChatWindowAction::Selecting) {
					auto&& Output = GetOutputRect();
					int Limit = (Output.y2 - Output.y1 - 10) / FontHeight;
					int Line = Limit - ((Output.y2 - 5) - Cursor.y) / FontHeight;
					int i = activeTab.Messages.size() - 1;
					int CurLine = Limit + 1;
					while (i >= 0) {
						auto&& cl = activeTab.Messages[i];
						if (CurLine - cl.GetLines() <= Line) {
							SelectionState.FromMsg = &cl;
							Action = ChatWindowAction::Selecting;
							auto Pos = CurLine - cl.GetLines() == Line ? 0 : cl.GetLineBreak(Line - (CurLine - cl.GetLines()) - 1)->nStartPos;
							int x = Cursor.x - (Output.x1 + 5);
							int Len = 0;
							while (x > Len && Pos < int(cl.ProcessedMsg.length())) {
								Len += GetTextLen(cl, Pos, 1);
								Pos++;
							}
							Pos--;
							if (Len - GetTextLen(cl, Pos, 1) / 2 > x) SelectionState.FromPos = Pos - 1;
							else SelectionState.FromPos = Pos;
							break;
						}
						CurLine -= cl.GetLines();
						i--;
					}
					if (i < 0) { SelectionState.FromMsg = 0; SelectionState.ToMsg = 0; }
				}
				else {
					auto&& Output = GetOutputRect();
					int Limit = (Output.y2 - Output.y1 - 10) / FontHeight;
					int Line = Limit - ((Output.y2 - 5) - Cursor.y) / FontHeight;
					int i = activeTab.Messages.size() - 1;
					int CurLine = Limit + 1;
					while (i >= 0) {
						auto&& cl = activeTab.Messages.at(i);
						if (CurLine - cl.GetLines() <= Line || i == 0) {
							SelectionState.ToMsg = &cl;
							int Pos;
							if (CurLine - cl.GetLines() <= Line) Pos = CurLine - cl.GetLines() == Line ? 0 : cl.GetLineBreak(Line - (CurLine - cl.GetLines()) - 1)->nStartPos;
							else Pos = 0;
							int x = Cursor.x - (Output.x1 + 5);
							int nLen = 0;
							while (x > nLen && Pos < int(cl.ProcessedMsg.length())) {
								nLen += GetTextLen(cl, Pos, 1);
								Pos++;
							}
							Pos--;
							if (nLen - GetTextLen(cl, Pos, 1) / 2 > x) SelectionState.ToPos = Pos - 1;
							else SelectionState.ToPos = Pos;
							break;
						}
						CurLine -= cl.GetLines();
						i--;
					}
				}
			}
			else if (Action != ChatWindowAction::Selecting) {
				SelectionState.FromMsg = 0;
				SelectionState.ToMsg = 0;
			}
		}
	}
	else
	{
		Action = ChatWindowAction::None;
		ResizeFlags = 0;
	}

	m_bLeftButtonDownLastFrame = MEvent::IsKeyDown(VK_LBUTTON);

	auto OutputRect = GetOutputRect();
	int VisibleLines = max(1, static_cast<int>((OutputRect.y2 - OutputRect.y1 - 10) / FontHeight));
	int MaxScrollOffset = max(0, activeTab.TotalLinesInHistory - VisibleLines);
	activeTab.ScrollOffsetLines = max(0, min(activeTab.ScrollOffsetLines, MaxScrollOffset));
}


void Chat::UpdateNewMessagesAnimation(float TimeDelta)
{
	if (ChatLinesPixelOffsetY <= 0) {
		return;
	}

	constexpr auto LinesPerSecond = 8;

	auto PixelDelta = TimeDelta * FontHeight * LinesPerSecond;
	ChatLinesPixelOffsetY -= PixelDelta;

	if (ChatLinesPixelOffsetY <= 0)
	{
		NumNewlyAddedLines--;
		ChatLinesPixelOffsetY = NumNewlyAddedLines > 0 ? FontHeight + ChatLinesPixelOffsetY : 0;
	}
}

D3DRECT Chat::GetOutputRect() {
	D3DRECT r = { Border.x1, Border.y1, Border.x2, Border.y2 - FontHeight };
	return r;
}

D3DRECT Chat::GetInputRect() {
	D3DRECT r = { Border.x1, Border.y2 - FontHeight, Border.x2, Border.y2 + (InputHeight - 1) * FontHeight };
	return r;
}

D3DRECT Chat::GetTotalRect() {
	D3DRECT r = { Border.x1, Border.y1 - 20, Border.x2, Border.y2 };
	return r;
}

static MRECT MakeMRECT(const D3DRECT & src)
{
	return{
		src.x1,
		src.y1,
		src.x2 - src.x1,
		src.y2 - src.y1,
	};
}

void Chat::OnDraw(MDrawContext* pDC)
{
	if (!m_bEmojisInitialized) {
		InitializeEmojis();
		m_bEmojisInitialized = true;
	}

	if (HideAlways ||
		(HideDuringReplays && ZGetGame()->IsReplay()))
		return;

	// Master override to completely hide chat if muted by the user.
	if (m_bNotificationsMuted && !IsInputEnabled()) {
		return;
	}

	auto Time = GetTime();

	bool isInputActive = IsInputEnabled();

	bool isMainChatFading = false;
	if (m_Tabs.count(to_lower_wstring(MAIN_TAB_NAME))) {
		const auto& mainTab = m_Tabs.at(to_lower_wstring(MAIN_TAB_NAME));
		for (const auto& msg : mainTab.Messages) {
			if (Time < msg.Time + FadeTime) {
				isMainChatFading = true;
				break;
			}
		}
	}
	bool isMainChatVisible = isInputActive || isMainChatFading;

	// Check if there are any unacknowledged whispers to display as vertical notifications.
	bool hasVisibleNotifications = false;
	for (const auto& pair : m_Tabs) {
		if (pair.first != to_lower_wstring(MAIN_TAB_NAME) && pair.second.UnreadCount > 0 && !pair.second.bHasBeenAcknowledged) {
			hasVisibleNotifications = true;
			break;
		}
	}

	// If nothing is visible or needs to be notified, exit.
	if (!isMainChatVisible && !hasVisibleNotifications) {
		return;
	}

	// Logic to switch between vertical notifications and the full chat window.
	if (!isInputActive && hasVisibleNotifications)
	{
		// PATH 1: Chat is "closed", only draw vertical notifications.
		DefaultFont.m_Font.BeginFont();

		int yPos = Border.y2 - FontHeight;
		const int xPos = Border.x1 + 5;
		const std::wstring main_key = to_lower_wstring(MAIN_TAB_NAME);

		// Loop through all tabs to find the ones that need a notification.
		for (const auto& pair : m_Tabs)
		{
			// Draw if the tab has unread messages AND has not been acknowledged yet.
			if (pair.first != main_key && pair.second.UnreadCount > 0 && !pair.second.bHasBeenAcknowledged)
			{
				std::wstring textToDraw = pair.second.Name + L" (" + std::to_wstring(pair.second.UnreadCount) + L")";

				u32 textColor = ARGB(255, 255, 255, 255);
				u32 bgColor = ARGB(220, 190, 160, 60);
				int textWidth = DefaultFont.GetWidth(textToDraw.c_str());

				pDC->SetColor(bgColor);
				//pDC->FillRectangle(xPos, yPos, textWidth + 10, FontHeight);

				DefaultFont.m_Font.DrawText(xPos + 5, yPos + 1, textToDraw.c_str(), textColor);

				yPos -= (FontHeight + 3);
			}
		}
		DefaultFont.m_Font.EndFont();
	}
	else
	{
		// PATH 2: Chat is active or fading, draw the full chat window.
		ChatTab& activeTab = GetActiveTab();
		if (activeTab.bLayoutIsDirty) {
			activeTab.LineSegments.clear();
			activeTab.TotalLinesInHistory = 0;
			for (size_t i = 0; i < activeTab.Messages.size(); ++i) {
				auto& chat_msg = activeTab.Messages[i];
				chat_msg.ProcessedMsg = chat_msg.OriginalMsg;
				chat_msg.FormatSpecifiers.clear();
				chat_msg.SubstituteFormatSpecifiers(m_EmojiMap);
				DivideIntoLines(activeTab, i, std::back_inserter(activeTab.LineSegments));
				activeTab.TotalLinesInHistory += chat_msg.GetLines();
			}
			activeTab.bLayoutIsDirty = false;
		}

		// Draw Tabs (Horizontally)
		int tabX = Border.x1 + 5;
		const int tabY = Border.y1 - 20;
		const int tabHeight = 20;
		const std::wstring main_key = to_lower_wstring(MAIN_TAB_NAME);

		DefaultFont.m_Font.BeginFont();

		if (isMainChatVisible && m_Tabs.count(main_key))
		{
			const auto& tabData = m_Tabs.at(main_key);
			const std::wstring& displayName = tabData.Name;
			int tabWidth = DefaultFont.GetWidth(displayName.c_str()) + 10;
			u32 textColor = TextColor;

			if (main_key == m_sActiveTabName) {
				u32 tabColor = ARGB(255, 0, 165, 195);
				textColor = ARGB(255, 255, 255, 255);
				pDC->SetColor(tabColor);
			}

			DefaultFont.m_Font.DrawText(tabX + 5, tabY + 2, displayName.c_str(), textColor);
			tabX += tabWidth + 2;
		}


		for (const auto& pair : m_Tabs) {
			const auto& key = pair.first;
			if (key == main_key) continue;

			const auto& tabData = pair.second;
			bool shouldDrawThisTab = (tabData.UnreadCount > 0) || isInputActive;
			if (!shouldDrawThisTab) continue;

			std::wstring textToDraw = tabData.Name;
			if (tabData.UnreadCount > 0) {
				textToDraw += L" (" + std::to_wstring(tabData.UnreadCount) + L")";
			}

			int tabWidth = DefaultFont.GetWidth(textToDraw.c_str()) + 10;
			u32 textColor = TextColor;

			if (key == m_sActiveTabName) {
				u32 tabColor = ARGB(255, 0, 165, 195);
				textColor = ARGB(255, 255, 255, 255);
				pDC->SetColor(tabColor);
			}
			else if (tabData.UnreadCount > 0) {
				u32 tabColor = ARGB(255, 190, 160, 60);
				textColor = ARGB(255, 255, 255, 255);
				pDC->SetColor(tabColor);
			}

			DefaultFont.m_Font.DrawText(tabX + 5, tabY + 2, textToDraw.c_str(), textColor);
			tabX += tabWidth + 2;
		}
		DefaultFont.m_Font.EndFont();

		if (isMainChatVisible) {
			bool ShowAll = ZIsActionKeyDown(ZACTION_SHOW_FULL_CHAT) && !InputEnabled;
			auto&& Output = GetOutputRect();
			int CeiledLimit, FlooredLimit;
			if (ShowAll) {
				CeiledLimit = FlooredLimit = (Output.y2 - 5) / FontHeight;
			}
			else {
				auto Limit = float(Output.y2 - Output.y1 - 10) / FontHeight;
				FlooredLimit = int(Limit);
				CeiledLimit = int(ceil(Limit));
			}
			DrawBackground(pDC, activeTab, Time, NumNewlyAddedLines > 0 ? CeiledLimit : FlooredLimit, ShowAll);
			DrawChatLines(pDC, activeTab, Time, isInputActive ? CeiledLimit : FlooredLimit, ShowAll);
			DrawSelection(pDC, activeTab);
			if (isInputActive) {
				DrawScrollbar(pDC, activeTab, FlooredLimit);
				DrawFrame(pDC, Time);
			}
		}
	}
}
int Chat::DrawTextWordWrap(MFontR2 & Font, const WStringView & Str, const D3DRECT & r, u32 Color)
{
	int Lines = 1;
	int StringLength = int(Str.size());
	int CurrentLineLength = 0;
	int MaxLineLength = r.x2 - r.x1;

	for (int i = 0; i < StringLength; i++)
	{
		int CharWidth = Font.GetWidth(&Str[i], 1);
		int CharHeight = Font.GetHeight();

		if (CurrentLineLength + CharWidth > MaxLineLength)
		{
			CurrentLineLength = 0;
			Lines++;
		}

		auto x = r.x1 + CurrentLineLength;
		auto y = r.y1 + (CharHeight + 1) * max(0, Lines - 1);
		Font.m_Font.DrawText(x, y,
			Str.substr(i, 1),
			Color);

		CurrentLineLength += CharWidth;
	}

	return Lines;
}

void Chat::DrawTextN(MFontR2 & pFont, const WStringView & Str, const D3DRECT & r, u32 Color)
{
	pFont.m_Font.DrawText(r.x1, r.y1, Str, Color);
}

void Chat::DrawBorder(MDrawContext * pDC)
{
	auto rect = Border;
	rect.y2 += (InputHeight - 1) * FontHeight;

	v2 vs[] = {
		{ float(rect.x1), float(rect.y1) },
		{ float(rect.x2), float(rect.y1) },
		{ float(rect.x2), float(rect.y2) },
		{ float(rect.x1), float(rect.y2) },
	};

	for (size_t i = 0; i < std::size(vs); i++)
	{
		auto a = vs[i];
		auto b = vs[(i + 1) % std::size(vs)];
		pDC->Line(a.x, a.y, b.x, b.y);
	}

	rect.y2 -= 2;
	rect.y2 -= InputHeight * FontHeight;
	pDC->Line(rect.x1, rect.y2, rect.x2, rect.y2);
}

void Chat::DrawBackground(MDrawContext * pDC, ChatTab & tab, TimeType Time, int Limit, bool ShowAll)
{
	if (BackgroundColor & 0xFF000000)
	{
		if (!InputEnabled)
		{
			int Lines = -max(0, NumNewlyAddedLines - 1);
			for (int i = int(tab.Messages.size() - 1); Lines < Limit && i >= 0; i--)
			{
				auto&& cl = tab.Messages.at(i);

				if (cl.Time + FadeTime < Time && !ShowAll && !InputEnabled)
					break;

				Lines += cl.GetLines();
			}

			Lines = min(Lines, Limit);

			if (Lines > 0)
			{
				auto&& Output = GetOutputRect();
				D3DRECT Rect = {
					Output.x1,
					Output.y2 - 5 - Lines * FontHeight,
					Output.x2,
					Output.y2,
				};

				if (NumNewlyAddedLines > 0) {
					Rect.y1 += ChatLinesPixelOffsetY;
					if (!ShowAll) {
						Rect.y1 = max(Rect.y1, Output.y1);
					}
				}

				pDC->SetColor(BackgroundColor);
				//pDC->FillRectangle(MakeMRECT(Rect));
			}
		}
		else
		{
			auto Rect = Border;
			Rect.y2 += (InputHeight - 1) * FontHeight;

			pDC->SetColor(BackgroundColor);
			//pDC->FillRectangle(MakeMRECT(Rect));
		}
	}
}

template <typename T>
struct LineDivisionState
{
	Chat* pChat; // ADDED
	T&& OutputIterator;
	LineSegmentInfo CurLineSegmentInfo;
	int ChatMessageIndex = 0;
	int MsgIndex = 0;
	int Lines = 0;
	int CurrentLinePixelLength = 0;
	u32 CurTextColor;
	u32 CurEmphasis = EmphasisType::Default;


	LineDivisionState(Chat* pChat, T&& OutputIterator, int ChatMessageIndex, u32 CurTextColor) :
		pChat{ pChat },
		OutputIterator{ std::forward<T>(OutputIterator) },
		ChatMessageIndex{ ChatMessageIndex },
		CurTextColor{ CurTextColor }
	{}

	void AddSegment(bool IsEndOfLine)
	{
		CurLineSegmentInfo.LengthInCharacters = MsgIndex - int(CurLineSegmentInfo.Offset);
		if (CurLineSegmentInfo.LengthInCharacters > 0) {
			OutputIterator++ = CurLineSegmentInfo;
		}

		if (IsEndOfLine)
		{
			CurrentLinePixelLength = 0;
			Lines++;
		}

		CurLineSegmentInfo = LineSegmentInfo{};
		CurLineSegmentInfo.ChatMessageIndex = ChatMessageIndex;
		CurLineSegmentInfo.Offset = MsgIndex;
		CurLineSegmentInfo.PixelOffsetX = CurrentLinePixelLength;
		CurLineSegmentInfo.IsStartOfLine = CurrentLinePixelLength == 0;
		CurLineSegmentInfo.TextColor = CurTextColor;
		CurLineSegmentInfo.Emphasis = CurEmphasis;
	}


	void HandleFormatSpecifier(FormatSpecifier& FormatSpec)
	{
		switch (FormatSpec.ft) {
		case FormatSpecifierType::Color:
			CurTextColor = FormatSpec.Color;
			break;

		case FormatSpecifierType::Default:
			CurEmphasis = EmphasisType::Default;
			break;

		case FormatSpecifierType::Bold:
			CurEmphasis |= EmphasisType::Bold;
			break;

		case FormatSpecifierType::Italic:
			CurEmphasis |= EmphasisType::Italic;
			break;

		case FormatSpecifierType::Underline:
			CurEmphasis |= EmphasisType::Underline;
			break;

		case FormatSpecifierType::Strikethrough:
			CurEmphasis |= EmphasisType::Strikethrough;
			break;

		case FormatSpecifierType::Linebreak:
			AddSegment(true);
			return;
		};

		if (MsgIndex - int(CurLineSegmentInfo.Offset) == 0)
		{
			CurLineSegmentInfo.TextColor = CurTextColor;
			CurLineSegmentInfo.Emphasis = CurEmphasis;
		}
		else
		{
			AddSegment(false);
		}
	}
};

template <typename T>
void Chat::DivideIntoLines(ChatTab & tab, int ChatMessageIndex, T && OutputIterator)
{
	auto&& cl = tab.Messages[ChatMessageIndex];
	cl.ClearWrappingLineBreaks();

	auto MaxLineLength = (Border.x2 - 5) - (Border.x1 + 5);

	LineDivisionState<T> State{ this, std::forward<T>(OutputIterator), ChatMessageIndex, cl.DefaultColor };

	State.CurLineSegmentInfo.ChatMessageIndex = ChatMessageIndex;
	State.CurLineSegmentInfo.Offset = 0;
	State.CurLineSegmentInfo.PixelOffsetX = 0;
	State.CurLineSegmentInfo.IsStartOfLine = true;
	State.CurLineSegmentInfo.TextColor = cl.DefaultColor;
	State.CurLineSegmentInfo.Emphasis = EmphasisType::Default;

	auto FormatIterator = cl.FormatSpecifiers.begin();
	for (State.MsgIndex = 0; State.MsgIndex < int(cl.ProcessedMsg.length()); ++State.MsgIndex)
	{
		bool bHandledAsObject = false;

		while (FormatIterator != cl.FormatSpecifiers.end() &&
			FormatIterator->nStartPos == State.MsgIndex)
		{
			if (FormatIterator->ft == FormatSpecifierType::Emoji) {
				State.AddSegment(false);

				auto it = m_EmojiMap.find(FormatIterator->EmojiName);
				if (it != m_EmojiMap.end()) {
					MBitmap* pBitmap = it->second;
					int EmojiHeight = FontHeight;
					int EmojiWidth = pBitmap->GetWidth() * (static_cast<float>(EmojiHeight) / pBitmap->GetHeight());

					if (State.CurrentLinePixelLength != 0 && State.CurrentLinePixelLength + EmojiWidth > MaxLineLength) {
						State.CurrentLinePixelLength = 0;
						State.Lines++;
					}

					LineSegmentInfo EmojiSegment{};
					EmojiSegment.Type = LineSegmentInfo::SegmentType::Emoji;
					EmojiSegment.pEmojiBitmap = pBitmap;
					EmojiSegment.ChatMessageIndex = ChatMessageIndex;
					EmojiSegment.Offset = State.MsgIndex;
					EmojiSegment.LengthInCharacters = 1;
					EmojiSegment.PixelOffsetX = State.CurrentLinePixelLength;
					EmojiSegment.IsStartOfLine = (State.CurrentLinePixelLength == 0);
					State.OutputIterator++ = EmojiSegment;

					State.CurrentLinePixelLength += EmojiWidth;

					State.CurLineSegmentInfo.Offset = State.MsgIndex + 1;
					State.CurLineSegmentInfo.PixelOffsetX = State.CurrentLinePixelLength;
				}

				bHandledAsObject = true;
			}
			else {
				State.HandleFormatSpecifier(*FormatIterator);
			}
			++FormatIterator;
		}

		if (bHandledAsObject) {
			continue;
		}

		auto CharWidth = DefaultFont.GetWidth(cl.ProcessedMsg.data() + State.MsgIndex, 1);

		if (State.CurrentLinePixelLength + CharWidth > MaxLineLength)
		{
			FormatIterator = std::next(cl.AddWrappingLineBreak(State.MsgIndex));
			State.AddSegment(true);
		}

		State.CurrentLinePixelLength += CharWidth;
	}

	State.AddSegment(true);

	cl.Lines = State.Lines;
}


MFontR2& Chat::GetFont(u32 Emphasis)
{
	if (Emphasis & EmphasisType::Italic)
		return ItalicFont;

	return DefaultFont;
}

static auto GetDrawLinesRect(const D3DRECT & OutputRect, int LinesDrawn,
	v2i PixelOffset, int FontHeight)
{
	return D3DRECT{
		OutputRect.x1 + 5 + PixelOffset.x,
		OutputRect.y2 - 5 - ((LinesDrawn + 1) * FontHeight) + PixelOffset.y,
		OutputRect.x2 - 5,
		OutputRect.y2 - 5
	};
}

static u32 ScaleAlpha(u32 Color, float MessageTime, float CurrentTime,
	float BeginFadeTime, float EndFadeTime)
{
	auto Delta = CurrentTime - MessageTime;

	auto A = (Color & 0xFF000000) >> 24;
	auto RGB = Color & 0x00FFFFFF;

	if (Delta < BeginFadeTime)
		return Color;
	if (Delta > EndFadeTime)
		return RGB;

	auto Scale = 1 - ((Delta - BeginFadeTime) / (EndFadeTime - BeginFadeTime));
	auto AS = static_cast<u8>(A * Scale);

	return (AS << 24) | RGB;
}

void Chat::DrawChatLines(MDrawContext * pDC, ChatTab & tab, TimeType Time, int Limit, bool ShowAll)
{
	auto Reverse = [&](auto&& Container, int Offset = 0) {
		return MakeRange(Container.rbegin() + Offset, Container.rend());
	};

	DefaultFont.m_Font.BeginFont();

	auto PrevClipRect = pDC->GetClipRect();
	{
		auto ClipRect = GetOutputRect();
		if (ShowAll)
			ClipRect.y1 = 0;
		pDC->SetClipRect(MakeMRECT(ClipRect));
	}

	if (ChatLinesPixelOffsetY > 0)
		Limit++;

	auto MessagesOffset = max(0, NumNewlyAddedLines - 1);

	int LinesDrawn = 0;
	for (auto&& LineSegment : Reverse(tab.LineSegments, MessagesOffset + tab.ScrollOffsetLines))
	{
		v2i PixelOffset{ LineSegment.PixelOffsetX, int(ChatLinesPixelOffsetY) };
		auto&& Rect = GetDrawLinesRect(GetOutputRect(), LinesDrawn, PixelOffset, FontHeight);
		auto&& cl = tab.Messages[LineSegment.ChatMessageIndex];

		if (!ShowAll && !InputEnabled && Time > cl.Time + FadeTime)
			break;

		if (LineSegment.Type == LineSegmentInfo::SegmentType::Emoji && LineSegment.pEmojiBitmap)
		{
			MBitmap* pBitmap = LineSegment.pEmojiBitmap;
			int EmojiHeight = FontHeight;
			int EmojiWidth = pBitmap->GetWidth() * (static_cast<float>(EmojiHeight) / pBitmap->GetHeight());

			int x = Rect.x1;
			int y = Rect.y1 + (FontHeight - EmojiHeight) / 2;

			auto Color = ScaleAlpha(0xFFFFFFFF, cl.Time, Time, FadeTime * 0.8f, FadeTime);
			pDC->SetBitmap(pBitmap);
			pDC->SetBitmapColor(Color);
			pDC->Draw(x, y, EmojiWidth, EmojiHeight);
			pDC->SetBitmapColor(0xFFFFFFFF);
		}
		else
		{
			auto String = cl.ProcessedMsg.data() + LineSegment.Offset;
			auto Length = LineSegment.LengthInCharacters;
			auto&& Font = GetFont(LineSegment.Emphasis);
			auto Color = LineSegment.TextColor;

			if (!ShowAll && !InputEnabled)
				Color = ScaleAlpha(Color, cl.Time, Time, FadeTime * 0.8f, FadeTime);

			DrawTextN(Font, { String, Length }, Rect, Color);
		}

		if (LineSegment.IsStartOfLine)
		{
			++LinesDrawn;
			if (LinesDrawn >= Limit)
				break;
		}
	}

	pDC->SetClipRect(PrevClipRect);
	DefaultFont.m_Font.EndFont();
}

void Chat::DrawSelection(MDrawContext * pDC, ChatTab & tab)
{
	if (SelectionState.FromMsg && SelectionState.ToMsg)
	{
		auto ret = GetPos(tab, *SelectionState.FromMsg, SelectionState.FromPos);
		if (!ret.first)
			return;
		auto From = ret.second;

		ret = GetPos(tab, *SelectionState.ToMsg, SelectionState.ToPos);
		if (!ret.first)
			return;
		auto To = ret.second;

		auto ShouldSwap = From.y > To.y || From.y == To.y && From.x > To.x;

		std::pair<const ChatMessage*, int> Stuff;
		if (ShouldSwap)
		{
			std::swap(From, To);
			Stuff = { SelectionState.FromMsg, SelectionState.FromPos };
		}
		else
		{
			Stuff = { SelectionState.ToMsg, SelectionState.ToPos };
		}

		ret = GetPos(tab, *Stuff.first, Stuff.second + 1);
		if (!ret.first)
			return;
		To = ret.second;

		auto Fill = [&](auto&&... Coords) {
			pDC->FillRectangle(MakeMRECT({ Coords... }));
		};

		pDC->SetColor(SelectionColor);

		auto TopOffset = int(ceil(FontHeight / 2.f));
		auto BottomOffset = FontHeight / 2;
		if (From.y == To.y)
		{
			Fill(From.x,
				From.y - TopOffset,
				To.x,
				To.y + BottomOffset);
		}
		else {
			Fill(From.x,
				From.y - TopOffset,
				Border.x2 - 5,
				From.y + BottomOffset);

			for (int i = FontHeight; i < To.y - From.y; i += FontHeight) {
				Fill(Border.x1 + 5,
					From.y + i - TopOffset,
					Border.x2 - 5,
					From.y + i + BottomOffset);
			}
			Fill(Border.x1,
				To.y - TopOffset,
				To.x - 5,
				To.y + BottomOffset);
		}
	}
}


void Chat::DrawFrame(MDrawContext * pDC, TimeType Time)
{
	{
		D3DRECT Rect = {
			Border.x1,
			Border.y1 - 20,
			Border.x2 + 1,
			Border.y1
		};

		pDC->SetColor(InterfaceColor);
		pDC->FillRectangle(MakeMRECT(Rect));
	}

	DrawBorder(pDC);

	{
		const int nIconWidth = 16;
		const int nIconHeight = 16;

		const int nIconX = Border.x1 + 5;
		const int nIconY = Border.y1 - 18;

		D3DRECT LockButtonRect = {
			nIconX,
			nIconY,
			nIconX + nIconWidth,
			nIconY + nIconHeight
		};

		const char* szIconName = m_bDragAndResizeEnabled ? "btn_chk.png" : "in_key.png";
		MBitmap* pIconBitmap = MBitmapManager::Get(szIconName);

		if (pIconBitmap)
		{
			pDC->SetBitmap(pIconBitmap);
			pDC->Draw(nIconX, nIconY, nIconWidth, nIconHeight);
		}
		else
		{
			const wchar_t* LockStateText = m_bDragAndResizeEnabled ? L"U" : L"L";
			DefaultFont.m_Font.DrawText(LockButtonRect.x1, LockButtonRect.y1, LockStateText, TextColor);
		}
	}

	{
		D3DRECT Rect = {
			Border.x2 - 15,
			Border.y1 - 18,
			Border.x2 - 15 + 12,
			Border.y1 - 18 + FontHeight,
		};
	}

	D3DRECT Rect = {
		Border.x1 + 5,
		Border.y2 - 2 - FontHeight,
		Border.x2,
		Border.y2,
	};

	int x = Rect.x1 + CaretCoord.x;
	int y = Rect.y1 + (CaretCoord.y - 1) * FontHeight;

	auto Period = Seconds(0.4f);
	if (Time % (Period * 2) > Period)
	{
		pDC->SetColor(TextColor);
		pDC->Line(x, y, x, y + FontHeight);
	}

	DrawTextWordWrap(DefaultFont, InputField.c_str(), Rect, TextColor);
}

void Chat::DrawScrollbar(MDrawContext * pDC, ChatTab & tab, int VisibleLines)
{
	if (tab.TotalLinesInHistory <= VisibleLines)
		return;

	const int ScrollbarWidth = 15;
	auto Output = GetOutputRect();
	D3DRECT TrackRect = {
		Border.x2,
		Output.y1,
		Border.x2 + ScrollbarWidth,
		Output.y2,
	};

	pDC->SetColor(ARGB(200, 50, 50, 50));
	pDC->FillRectangle(MakeMRECT(TrackRect));

	float TrackHeight = static_cast<float>(TrackRect.y2 - TrackRect.y1);
	float ThumbHeight = (static_cast<float>(VisibleLines) / tab.TotalLinesInHistory) * TrackHeight;
	ThumbHeight = max(ThumbHeight, 20.f);

	float ScrollPercentage = 0;
	if (tab.TotalLinesInHistory - VisibleLines > 0) {
		ScrollPercentage = static_cast<float>(tab.ScrollOffsetLines) / (tab.TotalLinesInHistory - VisibleLines);
	}

	float ThumbY = TrackRect.y1 + (1.0f - ScrollPercentage) * (TrackHeight - ThumbHeight);

	D3DRECT ThumbRect = {
		TrackRect.x1,
		static_cast<long>(ThumbY),
		TrackRect.x2,
		static_cast<long>(ThumbY + ThumbHeight),
	};

	pDC->SetColor(ARGB(200, 120, 120, 120));
	pDC->FillRectangle(MakeMRECT(ThumbRect));
}

void Chat::ResetFonts() {
	DefaultFont.Destroy();
	ItalicFont.Destroy();

	const auto Scale = 1.f;
	DefaultFont.Create("NewChatFont", FontName.c_str(),
		int(float(FontSize) / 1080 * RGetScreenHeight() + 0.5), Scale, BoldFont);
	ItalicFont.Create("NewChatItalicFont", FontName.c_str(),
		int(float(FontSize) / 1080 * RGetScreenHeight() + 0.5), Scale, BoldFont, true);

	FontHeight = DefaultFont.GetHeight();
}
