#include "stdafx.h"
#include "WriteMain.h"
#include "../../Common/PathUtil.h"
#include "../../Common/TSPacketUtil.h"

extern HINSTANCE g_instance;

CWriteMain::CWriteMain()
{
	this->file = INVALID_HANDLE_VALUE;
	this->writePlugin.hDll = NULL;

	WCHAR dllPath[MAX_PATH];
	DWORD ret = GetModuleFileName(g_instance, dllPath, MAX_PATH);
	if( ret && ret < MAX_PATH ){
		wstring iniPath = wstring(dllPath) + L".ini";
		wstring name = GetPrivateProfileToString(L"SET", L"WritePlugin", L"", iniPath.c_str());
		if( name.empty() == false && name[0] != L';' ){
			//出力プラグインを数珠繋ぎ
			this->writePlugin.Initialize(fs_path(iniPath).replace_filename(name).c_str());
		}
	}
}

CWriteMain::~CWriteMain()
{
	Stop();
	if( this->writePlugin.hDll ){
		this->writePlugin.pfnDeleteCtrl(this->writePlugin.id);
		FreeLibrary(this->writePlugin.hDll);
	}
}

BOOL CWriteMain::Start(
	LPCWSTR fileName,
	BOOL overWriteFlag,
	ULONGLONG createSize
	)
{
	Stop();

	this->savePath = fileName;
	this->targetSID = 0;
	fs_path name = fs_path(this->savePath).filename();
	if( name.c_str()[0] == L'#' ){
		//ファイル名が"#16進数4桁#"で始まるならこれをサービスIDと解釈して取り除く
		WCHAR* endp;
		WORD sid = (WORD)wcstol(name.c_str() + 1, &endp, 16);
		if( endp - name.c_str() == 5 && *endp == L'#' ){
			this->targetSID = sid == 0xFFFF ? 0 : sid;
			this->savePath = fs_path(this->savePath).replace_filename(name.c_str() + 6).native();
		}
	}

	if( this->writePlugin.hDll ){
		if( this->writePlugin.pfnStartSave(this->writePlugin.id, this->savePath.c_str(), overWriteFlag, createSize) == FALSE ){
			this->savePath = L"";
			return FALSE;
		}
		vector<WCHAR> path;
		DWORD pathSize = 0;
		if( this->writePlugin.pfnGetSaveFilePath(this->writePlugin.id, NULL, &pathSize) && pathSize > 0 ){
			path.resize(pathSize);
			if( this->writePlugin.pfnGetSaveFilePath(this->writePlugin.id, &path.front(), &pathSize) == FALSE ){
				path.clear();
			}
		}
		if( path.empty() ){
			this->writePlugin.pfnStopSave(this->writePlugin.id);
			this->savePath = L"";
			return FALSE;
		}
		this->savePath = &path.front();
	}else{
		_OutputDebugString(L"★CWriteMain::Start CreateFile:%s\r\n", this->savePath.c_str());
		UtilCreateDirectories(fs_path(this->savePath).parent_path());
		this->file = CreateFile(this->savePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL, overWriteFlag ? CREATE_ALWAYS : CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
		if( this->file == INVALID_HANDLE_VALUE ){
			_OutputDebugString(L"★CWriteMain::Start Err:0x%08X\r\n", GetLastError());
			fs_path pathWoExt = this->savePath;
			fs_path ext = pathWoExt.extension();
			pathWoExt.replace_extension();
			for( int i = 1; ; i++ ){
				Format(this->savePath, L"%s-(%d)%s", pathWoExt.c_str(), i, ext.c_str());
				this->file = CreateFile(this->savePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL, overWriteFlag ? CREATE_ALWAYS : CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
				if( this->file != INVALID_HANDLE_VALUE || i >= 999 ){
					DWORD err = GetLastError();
					_OutputDebugString(L"★CWriteMain::Start CreateFile:%s\r\n", this->savePath.c_str());
					if( this->file != INVALID_HANDLE_VALUE ){
						break;
					}
					_OutputDebugString(L"★CWriteMain::Start Err:0x%08X\r\n", err);
					this->savePath = L"";
					return FALSE;
				}
			}
		}
	}

	this->lastTSID = 0;
	this->packetInit.ClearBuff();
	this->catUtil = CCATUtil();
	this->pmtUtilMap.clear();
	CheckNeedPID();

	return TRUE;
}

BOOL CWriteMain::Stop(
	)
{
	if( this->file != INVALID_HANDLE_VALUE ){
		CloseHandle(this->file);
		this->file = INVALID_HANDLE_VALUE;
	}
	if( this->writePlugin.hDll ){
		this->writePlugin.pfnStopSave(this->writePlugin.id);
	}
	return TRUE;
}

wstring CWriteMain::GetSavePath(
	)
{
	return this->savePath;
}

BOOL CWriteMain::Write(
	BYTE* data,
	DWORD size,
	DWORD* writeSize
	)
{
	if( (this->writePlugin.hDll || this->file != INVALID_HANDLE_VALUE) && data != NULL && size > 0 && writeSize != NULL ){
		*writeSize = 0;
		if( this->targetSID == 0 ){
			//全サービスなので何も弄らない
			if( this->writePlugin.hDll ){
				if( this->writePlugin.pfnAddTSBuff(this->writePlugin.id, data, size, writeSize) == FALSE ){
					return FALSE;
				}
			}else{
				if( WriteFile(this->file, data, size, writeSize, NULL) == FALSE ){
					_OutputDebugString(L"★WriteFile Err:0x%08X\r\n", GetLastError());
					CloseHandle(this->file);
					this->file = INVALID_HANDLE_VALUE;
					return FALSE;
				}
			}
			return TRUE;
		}else{
			//指定サービス
			BYTE* outData;
			DWORD outSize;
			if( this->packetInit.GetTSData(data, size, &outData, &outSize) == FALSE ){
				outSize = 0;
			}
			//※ここからBonCtrl/CTSOut::AddTSBuff()とほとんど同じ作業
			this->outBuff.clear();
			for( DWORD i = 0; i < outSize; i += 188 ){
				CTSPacketUtil packet;
				if( packet.Set188TS(outData + i, 188) ){
					//指定サービスに必要なPIDを解析
					if( packet.transport_scrambling_control == 0 ){
						if( packet.PID == 1 ){
							//CAT
							if( this->catUtil.AddPacket(&packet) ){
								CheckNeedPID();
							}
						}
						if( packet.payload_unit_start_indicator && packet.data_byteSize > 0 ){
							BYTE pointer = packet.data_byte[0];
							if( 1 + pointer < packet.data_byteSize && packet.data_byte[1 + pointer] == 2 ){
								//PMT
								if( this->pmtUtilMap.count(packet.PID) == 0 ){
									this->pmtUtilMap[packet.PID] = CPMTUtil();
								}
								if( this->pmtUtilMap[packet.PID].AddPacket(&packet) ){
									CheckNeedPID();
								}
							}
						}else{
							//PMTの2パケット目かチェック
							if( this->pmtUtilMap.count(packet.PID) != 0 ){
								if( this->pmtUtilMap[packet.PID].AddPacket(&packet) ){
									CheckNeedPID();
								}
							}
						}
					}
					if( *std::lower_bound(this->needPIDList.begin(), this->needPIDList.end(), packet.PID) == packet.PID ){
						if( packet.PID == 0 ){
							//PATなので必要なサービスのみに絞る
							if( packet.payload_unit_start_indicator ){
								if( 5 < packet.data_byteSize ){
									//TSIDを取得
									this->lastTSID = packet.data_byte[4] << 8 | packet.data_byte[5];
									CheckNeedPID();
								}
								BYTE* patBuff;
								DWORD patBuffSize;
								if( this->lastTSID != 0 && this->patUtil.GetPacket(&patBuff, &patBuffSize) ){
									this->outBuff.insert(this->outBuff.end(), patBuff, patBuff + patBuffSize);
								}
							}
						}else{
							this->outBuff.insert(this->outBuff.end(), outData + i, outData + i + 188);
						}
					}
				}
			}
			if( this->outBuff.empty() ){
				*writeSize = size;
				return TRUE;
			}
			DWORD write;
			if( this->writePlugin.hDll ){
				if( this->writePlugin.pfnAddTSBuff(this->writePlugin.id, &this->outBuff.front(), (DWORD)this->outBuff.size(), &write) == FALSE ){
					return FALSE;
				}
			}else{
				if( WriteFile(this->file, &this->outBuff.front(), (DWORD)this->outBuff.size(), &write, NULL) == FALSE ){
					_OutputDebugString(L"★WriteFile Err:0x%08X\r\n", GetLastError());
					CloseHandle(this->file);
					this->file = INVALID_HANDLE_VALUE;
					return FALSE;
				}
			}
			*writeSize = size;
			return TRUE;
		}
	}
	return FALSE;
}

void CWriteMain::AddNeedPID(WORD pid)
{
	vector<WORD>::iterator itr = std::lower_bound(this->needPIDList.begin(), this->needPIDList.end(), pid);
	if( *itr != pid ){
		this->needPIDList.insert(itr, pid);
	}
}

void CWriteMain::CheckNeedPID()
{
	//0xFFFFは番兵
	this->needPIDList.assign(1, 0xFFFF);
	for( WORD i = 0; i <= 0x30; AddNeedPID(i++) );

	//PAT作成用のPMTリスト
	vector<pair<WORD, WORD>> pidList;
	//NITのPID追加しておく
	pidList.push_back(pair<WORD, WORD>(0x10, 0));

	//EMMのPID
	for( vector<WORD>::const_iterator itr = this->catUtil.GetPIDList().begin(); itr != this->catUtil.GetPIDList().end(); itr++ ){
		AddNeedPID(*itr);
	}
	for( map<WORD, CPMTUtil>::const_iterator itr = this->pmtUtilMap.begin(); itr != this->pmtUtilMap.end(); itr++ ){
		if( itr->second.GetProgramNumber() == this->targetSID ){
			//PAT作成用のPMTリスト作成
			pidList.push_back(std::make_pair(itr->first, itr->second.GetProgramNumber()));
			//指定サービスのPMT発見。PMT記載のPIDを登録
			AddNeedPID(itr->first);
			AddNeedPID(itr->second.GetPcrPID());
			for( auto jtr = itr->second.GetPIDTypeList().cbegin(); jtr != itr->second.GetPIDTypeList().end(); jtr++ ){
				AddNeedPID(jtr->first);
			}
		}
	}
	this->patUtil.SetParam(this->lastTSID, pidList);
}
