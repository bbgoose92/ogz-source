#pragma once
#include <vector>
#include <string>
#include <memory>
#include <map>
#include "MUtil.h"

constexpr u32 CHAT_DEFAULT_TEXT_COLOR = XRGB(0xC8, 0xC8, 0xC8);
constexpr u32 CHAT_DEFAULT_INTERFACE_COLOR = 0; 
constexpr u32 CHAT_DEFAULT_BACKGROUND_COLOR = 0;
constexpr u32 CHAT_DEFAULT_SELECTION_COLOR = ARGB(0xA0, 0, 0x80, 0xFF);

struct v2i
{
	int x, y;
};

enum class ChatWindowAction {
	None,
	Moving,
	Resizing,
	Selecting,
	Scrolling,
};

struct ChatMessage;
struct LineSegmentInfo;

struct ChatTab
{
	std::wstring Name;
	std::vector<ChatMessage> Messages;
	std::vector<LineSegmentInfo> LineSegments; // Each tab needs its own processed lines
	int UnreadCount = 0;
	int TotalLinesInHistory = 0;
	int ScrollOffsetLines = 0;
	bool bHasBeenAcknowledged = false;

	// A flag to know if this tab's layout needs to be recalculated
	bool bLayoutIsDirty = true;
};


class Chat
{
public:
	Chat(const std::string& FontName, bool Bold, int FontSize);
	~Chat();

	void EnableInput(bool Enable, bool ToTeam);
	void OutputChatMsg(const char* Msg);
	void OutputChatMsg(const char* Msg, u32 Color);

	void OnUpdate(float TimeDelta);
	void OnDraw(MDrawContext* pDC);
	bool OnEvent(MEvent* pEvent);

	void Scale(double WidthRatio, double HeightRatio);
	void Resize(int Width, int Height);

	void ClearHistory();

	const D3DRECT& GetRect() const { return Border; }
	void SetRect(D3DRECT& r) { Border = r; }
	void SetRect(int x1, int y1, int x2, int y2) { Border.x1 = x1; Border.y1 = y1; Border.x2 = x2; Border.y2 = y2; }

	auto GetFadeTime() const { return FadeTime; }
	void SetFadeTime(float fFade) { FadeTime = fFade; }

	const std::string& GetFont() const { return FontName; }
	int GetFontSize() const { return FontSize; }
	void SetFont(std::string s, bool b) { FontName = std::move(s); BoldFont = b; ResetFonts(); }
	void SetFontSize(int nSize) { FontSize = nSize; ResetFonts(); }

	D3DCOLOR GetTextColor() const { return TextColor; }
	D3DCOLOR GetInterfaceColor() const { return InterfaceColor; }
	D3DCOLOR GetBackgroundColor() const { return BackgroundColor; }
	void SetTextColor(D3DCOLOR Color) { TextColor = Color; }
	void SetBackgroundColor(D3DCOLOR Color) { BackgroundColor = Color; }
	void SetInterfaceColor(D3DCOLOR Color) { InterfaceColor = Color; }

	using TimeType = u64;
	static TimeType GetTime();
	static TimeType Seconds(float Value) { return TimeType(Value * 1000); }

	bool IsInputEnabled() const { return InputEnabled; }
	bool IsTeamChat() const { return TeamChat; }


	bool HideAlways{};
	bool HideDuringReplays{};

private:
	std::string FontName = "Arial";
	bool BoldFont = true;
	int FontSize = 16;
	int FontHeight{};
	TimeType FadeTime = Seconds(10);
	bool InputEnabled{};
	bool TeamChat{};
	MPOINT Cursor{};
	D3DRECT Border{};
	MFontR2 DefaultFont;
	MFontR2 ItalicFont;
	void InitializeEmojis();
	std::map<std::wstring, MBitmap*> m_EmojiMap;
	bool m_bEmojisInitialized = false;
	bool m_bLeftButtonDownLastFrame = false;

	u32 TextColor = CHAT_DEFAULT_TEXT_COLOR;
	u32 InterfaceColor = CHAT_DEFAULT_INTERFACE_COLOR;
	u32 BackgroundColor = CHAT_DEFAULT_BACKGROUND_COLOR;
	u32 SelectionColor = CHAT_DEFAULT_SELECTION_COLOR;
	ChatWindowAction Action = ChatWindowAction::None;
	u32 ResizeFlags{};
	struct SelectionStateType {
		const ChatMessage* FromMsg;
		int FromPos;
		const ChatMessage* ToMsg;
		int ToPos;
	} SelectionState{};
	std::vector<std::wstring> InputHistory;
	int CurInputHistoryEntry{};
	std::wstring InputField;
	int CaretPos = -1;

	int InputHeight{};
	v2i CaretCoord{};

	int NumNewlyAddedLines{};
	float ChatLinesPixelOffsetY{};
	
	TimeType m_LastMessageTime{};
	bool m_bDragAndResizeEnabled = false;
	bool m_bNotificationsMuted = false;

	std::map<std::wstring, ChatTab> m_Tabs;
	std::wstring m_sActiveTabName;
	static const std::wstring MAIN_TAB_NAME;

	ChatTab& GetActiveTab();
	const ChatTab& GetActiveTab() const;

	void UpdateNewMessagesAnimation(float TimeDelta);

	void DrawBorder(MDrawContext* pDC);
	void DrawBackground(MDrawContext* pDC, ChatTab& tab, TimeType Time, int Limit, bool ShowAll);
	void DrawChatLines(MDrawContext* pDC, ChatTab& tab, TimeType Time, int Limit, bool ShowAll);
	void DrawSelection(MDrawContext* pDC, ChatTab& tab);
	void DrawFrame(MDrawContext* pDC, TimeType Time);
	void DrawScrollbar(MDrawContext* pDC, ChatTab& tab, int VisibleLines);
	MFontR2& GetFont(u32 Emphasis);

	D3DRECT GetOutputRect();
	D3DRECT GetInputRect();
	D3DRECT GetTotalRect();
	template <typename T>
	void DivideIntoLines(ChatTab& tab, int ChatMessageIndex, T&& OutputIterator);
	std::pair<bool, v2i> GetPos(const ChatTab& tab, const ChatMessage& cl, u32 nPos);
	bool CursorInRange(int x, int y, int Width, int Height);
	int GetTextLen(ChatMessage& cl, int Pos, int Count);
	int GetTextLen(const char* Msg, int Count);

	int GetTextLength(MFontR2& Font, const wchar_t* Format, ...);
	int DrawTextWordWrap(MFontR2& Font, const WStringView& Str, const D3DRECT& r, u32 Color);
	void DrawTextN(MFontR2& Font, const WStringView& Str, const D3DRECT& r, u32 Color);

	void ResetFonts();
};
